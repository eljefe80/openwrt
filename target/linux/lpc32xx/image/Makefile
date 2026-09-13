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

# The kernel-bin here is a self-decompressing ARM zImage (KERNEL_NAME below).
# It MUST be wrapped as a comp=none uImage: U-Boot copies it verbatim to
# KERNEL_LOADADDR and jumps, and the zImage decompresses itself. Declaring
# comp=gzip makes bootm try (and fail) to gunzip the zImage, which aborts
# silently on this muted-console vendor U-Boot -- looks like a dead board.
define Build/uimage-none
	$(call Build/uImage,none)
endef

define Device/Default
	PROFILES := Default
	DEVICE_DTS_DIR := $(DTS_DIR)/nxp/lpc
	KERNEL_NAME := zImage
	KERNEL := kernel-bin | append-dtb
	KERNEL_INITRAMFS := kernel-bin | append-dtb
	FILESYSTEMS := squashfs
	BLOCKSIZE := 128k
	IMAGES := sysupgrade.bin
	IMAGE/sysupgrade.bin := append-kernel | pad-to 128k | append-rootfs | pad-rootfs | append-metadata
endef

# The one real device this target was built from. Everything past the
# kernel/DTB is unverified against real hardware -- see
# lpc3250-wattbox-wb300vb.dts for exactly which values are confirmed vs.
# best-effort. Do NOT flash sysupgrade.bin to a real unit yet: build and
# boot `<this>-initramfs-kernel.bin` over TFTP first (select "Initramfs" as
# the root filesystem in `make menuconfig`) -- that never touches NAND and
# is trivially recoverable by power-cycling back to the stock firmware.
# RAM/flash trimming is handled at the target level via DEVICE_TYPE=basic (see
# target/linux/lpc32xx/Makefile) so it also applies to the initramfs rootfs.
define Device/snapav_wattbox-wb300vb
	DEVICE_VENDOR := SnapAV
	DEVICE_MODEL := WattBox WB-300VB-IP-5
	DEVICE_DTS := lpc3250-wattbox-wb300vb
	# owut only, not luci-app-attendedsysupgrade -- unlike WHW03, this
	# device's own config deliberately strips dnsmasq/firewall4/nftables/
	# ppp/odhcpd to save space (see Seed target config in
	# build-wattbox.yml), so pulling in the full LuCI web framework just
	# for a one-button UI would fight that intent. owut alone is a
	# standalone ucode CLI with no LuCI/web dependency -- full self-update
	# capability without the bloat.
	#
	# wbctl added 2026-09-13: CONFIG_PACKAGE_wbctl=y in build-wattbox.yml's
	# Seed target config only gets it compiled/published into the package
	# feed, it does NOT make it a default for this profile -- confirmed by
	# reading this file directly (it was missing entirely) before this fix.
	# Without this, an ASU-requested WattBox image would build fine but
	# silently omit the outlet/LED/metering control daemon unless someone
	# explicitly requested it as an extra package. Matches RBS40V's
	# led-ring-mqtt, which genuinely is a DEVICE_PACKAGES default already.
	#
	# openwisp-config added same day, same root cause: CONFIG_PACKAGE_
	# openwisp-config=y in the Seed target config step only compiles it,
	# doesn't default it either -- found by checking all 4 devices after
	# the wbctl fix above.
	DEVICE_PACKAGES := owut wbctl openwisp-config
endef
TARGET_DEVICES += snapav_wattbox-wb300vb

$(eval $(call BuildImage))
