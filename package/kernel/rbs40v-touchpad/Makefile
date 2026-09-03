#
# Copyright (C) 2008-2010 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=rbs40v-touchpad
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0
PKG_SOURCE_URL:=https://github.com/eljefe80/rbs40v-touchpad.git
PKG_SOURCE_PROTO:=git
PKG_SOURCE_DATE:=2023-12-15
PKG_SOURCE_VERSION:=ce0c063df71479f9c17630004c20afa70f766d75

include $(INCLUDE_DIR)/package.mk

define KernelPackage/rbs40v-touchpad
  SUBMENU:=Input Devices
  TITLE:=Netgear Voice (RBS40v) Touchpad and LED support
  FILES:= \
	$(PKG_BUILD_DIR)/input-rbs40v-touchpad.ko
  AUTOLOAD:=$(call AutoProbe,input-rbs40v-touchpad,1)
  DEPENDS:=+kmod-input-core
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,rbs40v-touchpad))
