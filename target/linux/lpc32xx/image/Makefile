# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 OpenWrt.org

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

# UNVERIFIED: 0x80000000 (RAM base, confirmed via lpc32xx.dtsi/`mem=32M`)
# + 0x8000, the standard ARM Linux kernel load offset. This is the default
# almost every U-Boot ARM board ships with, but this specific device's
# U-Boot environment (loadaddr / bootcmd) has never been read -- there is
# no physical serial console access yet. Cross-check with `printenv` over
# a real serial connection before relying on this for a TFTP boot.
KERNEL_LOADADDR := 0x80008000

define Build/uimage-gzip
	$(call Build/uImage,gzip)
endef

define Device/Default
	PROFILES := Default
	DEVICE_DTS_DIR := $(DTS_DIR)/nxp/lpc
	KERNEL_NAME := zImage
	KERNEL := kernel-bin | append-dtb | uimage-gzip
	KERNEL_INITRAMFS := kernel-bin | append-dtb | uimage-gzip
	FILESYSTEMS := squashfs
	BLOCKSIZE := 128k
	IMAGES := sysupgrade.bin
	IMAGE/sysupgrade.bin := append-kernel | pad-to 128k | append-rootfs | pad-rootfs
endef

# The one real device this target was built from. Everything past the
# kernel/DTB is unverified against real hardware -- see
# lpc3250-wattbox-wb300vb.dts for exactly which values are confirmed vs.
# best-effort. Do NOT flash sysupgrade.bin to a real unit yet: build and
# boot `<this>-initramfs-kernel.bin` over TFTP first (select "Initramfs" as
# the root filesystem in `make menuconfig`) -- that never touches NAND and
# is trivially recoverable by power-cycling back to the stock firmware.
define Device/snapav_wattbox-wb300vb
	DEVICE_VENDOR := SnapAV
	DEVICE_MODEL := WattBox WB-300VB-IP-5
	DEVICE_DTS := lpc3250-wattbox-wb300vb
endef
TARGET_DEVICES += snapav_wattbox-wb300vb

$(eval $(call BuildImage))
