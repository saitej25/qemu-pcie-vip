/* SPDX-License-Identifier: MIT */
#ifndef PCIE_VIP_UAPI_H
#define PCIE_VIP_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct pcie_vip_run_config {
	__u32 length;
	__u32 iterations;
	__u32 completed;
	__u32 last_status;
	__u64 elapsed_ns;
};

#define PCIE_VIP_IOC_RUN _IO('V', 0)
#define PCIE_VIP_IOC_RUN_CONFIG _IOWR('V', 1, struct pcie_vip_run_config)

#endif
