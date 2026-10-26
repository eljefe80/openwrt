include $(TOPDIR)/rules.mk

PKG_NAME:=asrock-x10-ir
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/asrock-x10-ir
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=ASRock X10 IR blaster control utility
  DEPENDS:=@TARGET_ipq40xx +kmod-i2c-gpio
endef

define Package/asrock-x10-ir/description
  x10-ir talks to the ASRock X10's IR blaster MCU over I2C: learn and
  send IR codes, read the MCU firmware version, set the MCU-driven LED.
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-Wall -o $(PKG_BUILD_DIR)/x10-ir ./src/x10-ir.c
endef

define Package/asrock-x10-ir/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/x10-ir $(1)/usr/sbin/x10-ir
endef

$(eval $(call BuildPackage,asrock-x10-ir))
