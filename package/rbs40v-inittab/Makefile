#
# Patches ipq40xx's shared /etc/inittab for the RBS40V only, dropping the
# ttyMSM1 askfirst entry. ttyMSM1 is this board's CSR8811 Bluetooth UART
# (see etc/init.d/rbs40v-bluetooth in target/linux/ipq40xx/base-files); the
# generic ipq40xx inittab spawns a console getty on it, which contends with
# bccmd/hciattach for the same port. A uci-defaults runtime fix can't work
# here -- procd parses /etc/inittab once at its own PID1 startup, before any
# uci-defaults script ever runs -- and apk's package model refuses to let a
# second package simply ship a competing /etc/inittab (it errors: "trying
# to overwrite etc/inittab owned by base-files"), so this uses a postinst
# script to patch the file base-files already installed, instead of trying
# to replace it outright. postinst runs both at a real device's opkg/apk
# install AND at build time when this image's rootfs is first assembled
# (via the standard $IPKG_INSTROOT staging-root convention), so the fix
# lands before procd ever reads the file, on the very first boot.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=rbs40v-inittab
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define Package/rbs40v-inittab
  SECTION:=utils
  CATEGORY:=Base system
  DEPENDS:=+base-files
  TITLE:=Frees ttyMSM1 (RBS40V Bluetooth UART) from the console getty
endef

define Package/rbs40v-inittab/description
 Patches the shared ipq40xx /etc/inittab to drop the ttyMSM1 console getty
 entry, since ttyMSM1 is the RBS40V's CSR8811 Bluetooth UART, not a
 console.
endef

define Build/Compile
endef

define Package/rbs40v-inittab/install
	$(INSTALL_DIR) $(1)
endef

define Package/rbs40v-inittab/postinst
#!/bin/sh
sed -i '/^ttyMSM1::askfirst:/d' "$${IPKG_INSTROOT}/etc/inittab"
exit 0
endef

$(eval $(call BuildPackage,rbs40v-inittab))
