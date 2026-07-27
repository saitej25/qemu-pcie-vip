################################################################################
# pcie-vip guest utilities
################################################################################

PCIE_VIP_VERSION = local
PCIE_VIP_SITE = $(BR2_EXTERNAL_PCIE_VIP_PATH)/..
PCIE_VIP_SITE_METHOD = local

define PCIE_VIP_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/user CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS) -D_FILE_OFFSET_BITS=64"
endef

define PCIE_VIP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/user/pcie-vip-pcimem $(TARGET_DIR)/usr/bin/pcie-vip-pcimem
	$(INSTALL) -D -m 0755 $(@D)/user/pcie-vip-run $(TARGET_DIR)/usr/bin/pcie-vip-run
	$(INSTALL) -D -m 0755 $(@D)/scripts/run_guest_checks.sh $(TARGET_DIR)/usr/bin/pcie-vip-guest-checks
endef

$(eval $(generic-package))
