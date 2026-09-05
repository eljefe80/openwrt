#
# bccmd -- the CSR/BlueCore vendor command-and-pskey tool from BlueZ 4.x
# (tools/bccmd.c). Dropped entirely from BlueZ 5, even under
# --enable-deprecated (bluez-utils here only restores hciattach/hciconfig/
# hcitool/hcidump). Built standalone from real upstream BlueZ 4.101 source
# against BlueZ 4's own private libbluetooth sources (lib/bluetooth.c,
# hci.c, sdp.c, uuid.c) -- NOT this system's installed BlueZ 5
# libbluetooth, which removed/changed functions bccmd depends on (matches
# upstream's own build: tools/bccmd links lib/libbluetooth-private.la, a
# separate copy of the same lib/ sources, not the installed shared lib).
# USB transport is left out (only BCSP/H4/3-wire), so no libusb dependency.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=bccmd
PKG_VERSION:=4.101
PKG_RELEASE:=1

PKG_SOURCE:=bluez-$(PKG_VERSION).tar.xz
PKG_SOURCE_URL:=@KERNEL/linux/bluetooth
PKG_HASH:=41f9578bef39b8c94a2d6ddeaa556afd22d136936d0f03100e422fe970a45a7d
PKG_BUILD_DIR:=$(BUILD_DIR)/bluez-$(PKG_VERSION)

PKG_LICENSE:=GPL-2.0
PKG_MAINTAINER:=Jeff Williams <jeff@wdwconsulting.net>

include $(INCLUDE_DIR)/package.mk

define Package/bccmd
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=CSR BlueCore vendor command tool (bccmd, from BlueZ 4.x)
endef

define Package/bccmd/description
 bccmd, the CSR/BlueCore vendor command-and-pskey tool from BlueZ 4.x.
 Needed to bring up CSR8x11-family Bluetooth chips (persistent-store key
 loading over BCSP) that bluez5's hciattach/hciconfig alone can't
 initialize. Built from real upstream BlueZ 4.101 source, not a vendored
 binary.
endef

BCCMD_TOOLS_SRCS:=bccmd.c csr.c csr_hci.c csr_h4.c csr_3wire.c csr_bcsp.c ubcsp.c
BCCMD_LIB_SRCS:=bluetooth.c hci.c sdp.c uuid.c
BCCMD_BUILD:=$(PKG_BUILD_DIR)/bccmd-build

define Build/Compile
	rm -rf $(BCCMD_BUILD)
	mkdir -p $(BCCMD_BUILD)/bluetooth
	$(CP) $(PKG_BUILD_DIR)/lib/*.h $(BCCMD_BUILD)/
	$(CP) $(PKG_BUILD_DIR)/lib/*.h $(BCCMD_BUILD)/bluetooth/
	$(foreach f,$(BCCMD_LIB_SRCS),$(CP) $(PKG_BUILD_DIR)/lib/$(f) $(BCCMD_BUILD)/;)
	$(foreach f,$(BCCMD_TOOLS_SRCS),$(CP) $(PKG_BUILD_DIR)/tools/$(f) $(BCCMD_BUILD)/;)
	$(CP) $(PKG_BUILD_DIR)/tools/csr.h $(PKG_BUILD_DIR)/tools/ubcsp.h $(BCCMD_BUILD)/
	echo '#define VERSION "$(PKG_VERSION)"' > $(BCCMD_BUILD)/config.h
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
		-I$(BCCMD_BUILD) \
		-o $(BCCMD_BUILD)/bccmd \
		$(addprefix $(BCCMD_BUILD)/,$(BCCMD_TOOLS_SRCS) $(BCCMD_LIB_SRCS)) \
		$(TARGET_LDFLAGS)
endef

define Package/bccmd/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(BCCMD_BUILD)/bccmd $(1)/usr/sbin/bccmd
endef

$(eval $(call BuildPackage,bccmd))
