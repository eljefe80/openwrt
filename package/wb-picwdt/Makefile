include $(TOPDIR)/rules.mk

PKG_NAME:=wb-picwdt
PKG_VERSION:=1.0
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/wb-picwdt
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=WattBox external PIC watchdog keepalive daemon
  DEPENDS:=@TARGET_lpc32xx
endef

define Package/wb-picwdt/description
  Feeds the WattBox WB-300VB-IP-5's external PIC watchdog by reproducing
  the vendor firmware's GPIO bit-bang protocol on P0.6/P0.7 (see
  picwdt-waveform-RE.md in the bring-up project notes). Without this, the
  PIC resets the board ~30s after boot on any non-vendor kernel -- this
  daemon is what makes staying up past that point possible at all.
  Launched from a preinit hook (06_wb_picwdt) that deliberately runs
  BEFORE the failsafe-mode keypress wait, so feeding starts as early in
  boot as possible -- ordering this After the failsafe wait was the
  original cause of the ~30s deadline being missed (see SESSION-NOTES.md,
  2026-08-20 "Concern #2c").
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(PKG_BUILD_DIR)/wb-picwdt $(PKG_BUILD_DIR)/picwdt-keepalive.c
endef

define Package/wb-picwdt/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/wb-picwdt $(1)/usr/sbin/wb-picwdt
	$(INSTALL_DIR) $(1)/lib/preinit
	$(INSTALL_DATA) ./files/06_wb_picwdt $(1)/lib/preinit/06_wb_picwdt
endef

$(eval $(call BuildPackage,wb-picwdt))
