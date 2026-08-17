# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 OpenWrt.org

include $(TOPDIR)/rules.mk

ARCH:=arm
BOARD:=lpc32xx
BOARDNAME:=NXP LPC32x0
FEATURES:=squashfs nand dt gpio rtc
SUBTARGETS:=generic
CPU_TYPE:=arm926ej-s

KERNEL_PATCHVER:=6.12

include $(INCLUDE_DIR)/target.mk

KERNELNAME:=zImage dtbs

define Target/Description
	Build firmware images for the NXP LPC32x0 (ARM926EJ-S) SoC family,
	reverse-engineered from a SnapAV/WattBox IP-series power conditioner.
	See the board DTS for hardware verification status.
endef

$(eval $(call BuildTarget))
