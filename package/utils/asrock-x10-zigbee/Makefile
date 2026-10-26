include $(TOPDIR)/rules.mk

PKG_NAME:=asrock-x10-zigbee
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/asrock-x10-zigbee
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=ASRock X10 Zigbee radio (CC2630 ZNP) over ser2net
  DEPENDS:=@TARGET_ipq40xx +gpiod-tools +ser2net
  PKGARCH:=all
endef

define Package/asrock-x10-zigbee/description
  Resets the ASRock X10's TI CC2630 Zigbee NCP at boot and exposes it on
  TCP port 6638 via ser2net for Home Assistant (ZHA / Zigbee2MQTT).
  "x10-zigbee bootloader" enters the CC2630 ROM bootloader for reflashing.
endef

define Build/Compile
endef

define Package/asrock-x10-zigbee/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/x10-zigbee $(1)/usr/sbin/x10-zigbee
	$(INSTALL_BIN) ./files/x10-zigbee.init $(1)/etc/init.d/x10-zigbee
	$(INSTALL_DATA) ./files/x10-zigbee.uci-defaults $(1)/etc/uci-defaults/90-x10-zigbee
endef

# The generic ipq40xx inittab runs a second login console on ttyMSM1, which
# is the CC2630's UART: it writes its prompt to the radio and swallows the
# radio's replies (confirmed on hardware: SYS_PING only answers once that
# askfirst is stopped). procd reads inittab before uci-defaults run, so drop
# the line at image build time rather than on first boot.
define Package/asrock-x10-zigbee/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || [ "$$(board_name)" = "asrock,x10" ] || exit 0
sed -i '/^ttyMSM1::askfirst:/d' "$${IPKG_INSTROOT}/etc/inittab"
exit 0
endef

$(eval $(call BuildPackage,asrock-x10-zigbee))
