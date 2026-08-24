include $(TOPDIR)/rules.mk

PKG_NAME:=wbctl
PKG_VERSION:=1
PKG_RELEASE:=1
PKG_MAINTAINER:=Jeff Williams

include $(INCLUDE_DIR)/package.mk

define Package/wbctl
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=WattBox WB-300VB-IP-5 control daemon (MQTT/HA)
  DEPENDS:=+libmosquitto-nossl
endef

define Package/wbctl/description
 Drives the WattBox PIC (outlets, LEDs, voltage/current) over /dev/ttyS1 and
 bridges it to MQTT with Home Assistant discovery. Config via UCI (/etc/config/wbctl).
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -Wall -O2 \
		-o $(PKG_BUILD_DIR)/wbctld $(PKG_BUILD_DIR)/wbctld.c \
		$(TARGET_LDFLAGS) -lmosquitto
endef

define Package/wbctl/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/config $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/wbctld $(1)/usr/sbin/wbctld
	$(INSTALL_CONF) ./files/wbctl.config $(1)/etc/config/wbctl
	$(INSTALL_BIN) ./files/wbctl.init $(1)/etc/init.d/wbctl
endef

$(eval $(call BuildPackage,wbctl))
