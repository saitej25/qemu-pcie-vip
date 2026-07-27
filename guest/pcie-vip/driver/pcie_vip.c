// SPDX-License-Identifier: GPL-2.0
/* Minimal Linux driver for the standalone QEMU pcie-vip endpoint. */
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define PCIE_VIP_VENDOR 0x1234
#define PCIE_VIP_DEVICE 0x11e9

#define VIP_CAP 0x0000
#define VIP_CC 0x0014
#define VIP_CSTS 0x001c
#define VIP_AQA 0x0024
#define VIP_ASQ 0x0028
#define VIP_ACQ 0x0030
#define VIP_SQ0TDBL 0x1000
#define VIP_DMA_DESC_BASE_LO 0x1040
#define VIP_DMA_DESC_BASE_HI 0x1044
#define VIP_DMA_CPL_BASE_LO  0x1048
#define VIP_DMA_CPL_BASE_HI  0x104c
#define VIP_DMA_DESC_COUNT   0x1050
#define VIP_DMA_DESC_TAIL    0x1054
#define VIP_DMA_STATUS       0x1058
#define VIP_DMA_CONTROL      0x105c

#define VIP_CC_EN BIT(0)
#define VIP_CSTS_RDY BIT(0)
#define VIP_AQA_VALUE ((31U << 16) | 31U)

#define PCIE_VIP_IOC_RUN _IO('V', 0)

struct pcie_vip_dev {
	struct pci_dev *pdev;
	void __iomem *bar0;
	void *cmd;
	dma_addr_t cmd_dma;
	void *completion;
	dma_addr_t completion_dma;
	struct completion irq_done;
	struct mutex run_lock;
	struct miscdevice misc;
};

struct pcie_vip_dma_desc {
	__le64 host_addr;
	__le32 ram_addr;
	__le16 length;
	u8 opcode;
	u8 flags;
	__le32 tag;
	__le32 reserved;
	__le64 reserved2;
};

struct pcie_vip_dma_cpl {
	__le32 tag;
	__le16 status;
	__le16 reserved;
	__le32 bytes;
	__le32 sequence;
};

static irqreturn_t pcie_vip_irq(int irq, void *opaque)
{
	struct pcie_vip_dev *vip = opaque;

	complete(&vip->irq_done);
	return IRQ_HANDLED;
}

static int pcie_vip_run(struct pcie_vip_dev *vip)
{
	unsigned int i;
	unsigned long timeout;
	int ret = 0;
	struct pcie_vip_dma_desc *desc = NULL;
	struct pcie_vip_dma_cpl *cpl = NULL;
	void *src = NULL, *dst = NULL;
	dma_addr_t desc_dma, cpl_dma, src_dma, dst_dma;

	desc = dma_alloc_coherent(&vip->pdev->dev, 2 * sizeof(*desc),
				  &desc_dma, GFP_KERNEL);
	cpl = dma_alloc_coherent(&vip->pdev->dev, 2 * sizeof(*cpl),
				&cpl_dma, GFP_KERNEL);
	src = dma_alloc_coherent(&vip->pdev->dev, 64, &src_dma, GFP_KERNEL);
	dst = dma_alloc_coherent(&vip->pdev->dev, 64, &dst_dma, GFP_KERNEL);
	if (!desc || !cpl || !src || !dst) {
		ret = -ENOMEM;
		goto free_dma;
	}

	mutex_lock(&vip->run_lock);
	memset(cpl, 0, 2 * sizeof(*cpl));
	memset(dst, 0, 64);
	for (i = 0; i < 64; ++i)
		((u8 *)src)[i] = 0xa0 + i;
	memset(desc, 0, 2 * sizeof(*desc));
	desc[0].host_addr = cpu_to_le64(src_dma);
	desc[0].ram_addr = cpu_to_le32(0);
	desc[0].length = cpu_to_le16(64);
	desc[0].opcode = 0;
	desc[0].tag = cpu_to_le32(0x100);
	desc[1].host_addr = cpu_to_le64(dst_dma);
	desc[1].ram_addr = cpu_to_le32(0);
	desc[1].length = cpu_to_le16(64);
	desc[1].opcode = 1;
	desc[1].flags = 1;
	desc[1].tag = cpu_to_le32(0x101);

	iowrite32(lower_32_bits(desc_dma), vip->bar0 + VIP_DMA_DESC_BASE_LO);
	iowrite32(upper_32_bits(desc_dma), vip->bar0 + VIP_DMA_DESC_BASE_HI);
	iowrite32(lower_32_bits(cpl_dma), vip->bar0 + VIP_DMA_CPL_BASE_LO);
	iowrite32(upper_32_bits(cpl_dma), vip->bar0 + VIP_DMA_CPL_BASE_HI);
	iowrite32(2, vip->bar0 + VIP_DMA_DESC_COUNT);
	iowrite32(1, vip->bar0 + VIP_DMA_CONTROL);

	for (i = 0; i < 50; ++i) {
		if (ioread32(vip->bar0 + VIP_DMA_STATUS) == 0)
			break;
		msleep(1);
	}
	if (i == 50) {
		ret = -ETIMEDOUT;
		goto out;
	}

	reinit_completion(&vip->irq_done);
	iowrite32(2, vip->bar0 + VIP_DMA_DESC_TAIL);
	timeout = wait_for_completion_timeout(&vip->irq_done, 5 * HZ);
	if (!timeout) {
		ret = -ETIMEDOUT;
		goto out;
	}
	for (i = 0; i < 64; ++i) {
		if (((u8 *)dst)[i] != (u8)(0xa0 + i)) {
			ret = -EIO;
			break;
		}
	}
	if (le32_to_cpu(cpl[0].tag) != 0x100 || le16_to_cpu(cpl[0].status) != 0 ||
	    le32_to_cpu(cpl[0].bytes) != 64 || le32_to_cpu(cpl[1].tag) != 0x101 ||
	    le16_to_cpu(cpl[1].status) != 0 || le32_to_cpu(cpl[1].bytes) != 64)
		ret = -EIO;
out:
	mutex_unlock(&vip->run_lock);

free_dma:
	if (dst)
		dma_free_coherent(&vip->pdev->dev, 64, dst, dst_dma);
	if (src)
		dma_free_coherent(&vip->pdev->dev, 64, src, src_dma);
	if (cpl)
		dma_free_coherent(&vip->pdev->dev, 2 * sizeof(*cpl), cpl, cpl_dma);
	if (desc)
		dma_free_coherent(&vip->pdev->dev, 2 * sizeof(*desc), desc, desc_dma);
	return ret;
}

static long pcie_vip_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	struct miscdevice *misc = file->private_data;
	struct pcie_vip_dev *vip = container_of(misc, struct pcie_vip_dev, misc);

	if (cmd != PCIE_VIP_IOC_RUN)
		return -ENOTTY;
	return pcie_vip_run(vip);
}

static const struct file_operations pcie_vip_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = pcie_vip_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = pcie_vip_ioctl,
#endif
};

static int pcie_vip_probe(struct pci_dev *pdev,
				  const struct pci_device_id *id)
{
	struct pcie_vip_dev *vip;
	int ret;

	vip = devm_kzalloc(&pdev->dev, sizeof(*vip), GFP_KERNEL);
	if (!vip)
		return -ENOMEM;
	vip->pdev = pdev;
	mutex_init(&vip->run_lock);
	init_completion(&vip->irq_done);
	pci_set_drvdata(pdev, vip);

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	ret = pcim_iomap_regions(pdev, BIT(0), "pcie_vip");
	if (ret)
		return ret;
	vip->bar0 = pcim_iomap_table(pdev)[0];
	if (!vip->bar0)
		return -ENODEV;
	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);
	if (ret < 0)
		return ret;
	ret = devm_request_irq(&pdev->dev, pci_irq_vector(pdev, 0),
				       pcie_vip_irq, 0, "pcie_vip", vip);
	if (ret)
		goto err_vectors;

	vip->cmd = dma_alloc_coherent(&pdev->dev, 64, &vip->cmd_dma, GFP_KERNEL);
	vip->completion = dma_alloc_coherent(&pdev->dev, 16,
					     &vip->completion_dma, GFP_KERNEL);
	if (!vip->cmd || !vip->completion) {
		ret = -ENOMEM;
		goto err_dma;
	}

	vip->misc.minor = MISC_DYNAMIC_MINOR;
	vip->misc.name = "pcie_vip0";
	vip->misc.fops = &pcie_vip_fops;
	vip->misc.parent = &pdev->dev;
	ret = misc_register(&vip->misc);
	if (ret)
		goto err_dma;

	dev_info(&pdev->dev, "pcie-vip ready: /dev/%s\n", vip->misc.name);
	return 0;

err_dma:
	if (vip->completion)
		dma_free_coherent(&pdev->dev, 16, vip->completion,
					  vip->completion_dma);
	if (vip->cmd)
		dma_free_coherent(&pdev->dev, 64, vip->cmd, vip->cmd_dma);
err_vectors:
	pci_free_irq_vectors(pdev);
	return ret;
}

static void pcie_vip_remove(struct pci_dev *pdev)
{
	struct pcie_vip_dev *vip = pci_get_drvdata(pdev);

	misc_deregister(&vip->misc);
	/* devm_request_irq() is released after .remove() returns.  Release it
	 * before tearing down the MSI-X vectors so the PCI core never observes a
	 * live IRQ using an already-freed vector table. */
	devm_free_irq(&pdev->dev, pci_irq_vector(pdev, 0), vip);
	dma_free_coherent(&pdev->dev, 16, vip->completion, vip->completion_dma);
	dma_free_coherent(&pdev->dev, 64, vip->cmd, vip->cmd_dma);
	pci_free_irq_vectors(pdev);
}

static const struct pci_device_id pcie_vip_ids[] = {
	{ PCI_DEVICE(PCIE_VIP_VENDOR, PCIE_VIP_DEVICE) },
	{ }
};
MODULE_DEVICE_TABLE(pci, pcie_vip_ids);

static struct pci_driver pcie_vip_driver = {
	.name = "pcie_vip",
	.id_table = pcie_vip_ids,
	.probe = pcie_vip_probe,
	.remove = pcie_vip_remove,
};
module_pci_driver(pcie_vip_driver);

MODULE_DESCRIPTION("Standalone QEMU PCIe VIP test driver");
MODULE_LICENSE("GPL");
