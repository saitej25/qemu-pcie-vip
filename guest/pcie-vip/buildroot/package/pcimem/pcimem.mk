################################################################################
# billfarrow/pcimem userspace PCI BAR utility
################################################################################

PCIMEM_VERSION = 09724edb1783a98da2b7ae53c5aaa87493aabc9b
PCIMEM_SITE = $(BR2_EXTERNAL_PCIE_VIP_PATH)/../../../third_party/pcimem
PCIMEM_SITE_METHOD = local

define PCIMEM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -D_FILE_OFFSET_BITS=64 \
		-o $(@D)/pcimem $(@D)/pcimem.c
endef

define PCIMEM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pcimem $(TARGET_DIR)/usr/bin/pcimem
endef

$(eval $(generic-package))
