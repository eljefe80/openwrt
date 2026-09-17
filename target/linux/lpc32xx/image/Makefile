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

# mtd1 (kernel, 0x300000) and mtd2 (rootfs, 0x1b0000) are separate, fixed-size
# NOR partitions -- confirmed by reading the device's own MTD partition table
# over serial. platform_do_upgrade() splits sysupgrade.bin at the squashfs
# magic and writes each half to its own mtd, so a combined check-size on the
# concatenated image can't tell which half overflowed: the kernel here only
# uses ~2.2MB of its 3MB budget, and that unused slack would silently absorb
# part of a rootfs overage in any single combined-size check. check-size
# below is scoped to $(IMAGE_ROOTFS) alone -- the real squashfs data, before
# concatenation -- so it fails loudly on the exact failure mode hit on real
# hardware 2026-09-14: a rootfs build 488KB over mtd2's actual capacity that
# `mtd write` only caught mid-flash, as "Failed to erase block".
KERNEL_SIZE := 3145728
ROOTFS_SIZE := 1769472

define Build/check-wattbox-rootfs-size
	@actual="$$(stat -c%s $(IMAGE_ROOTFS))"; \
	limit=$(ROOTFS_SIZE); \
	[ "$$actual" -le "$$limit" ] || { \
		$(call ERROR_MESSAGE,    ERROR: WattBox rootfs is too big: $$actual > $$limit \(mtd2 capacity\)); \
		rm -f $@; \
		exit 1; \
	}
endef

define Device/Default
	PROFILES := Default
	DEVICE_DTS_DIR := $(DTS_DIR)/nxp/lpc
	KERNEL_NAME := zImage
	KERNEL := kernel-bin | append-dtb
	KERNEL_INITRAMFS := kernel-bin | append-dtb
	FILESYSTEMS := squashfs initramfs
	BLOCKSIZE := 128k
	IMAGES := sysupgrade.bin
	IMAGE/sysupgrade.bin := check-wattbox-rootfs-size | append-kernel | pad-to 128k | append-rootfs | pad-rootfs | append-metadata
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
	#
	# lldpd added 2026-09-14 (opensoho reported "No LLDP data, verify
	# whether lldpd is installed", same gap class added to all 4 managed
	# devices at once), DROPPED again 2026-09-15: this device's real
	# rootfs budget is 1,769,472 bytes and every byte matters more than
	# LLDP visibility does here -- see the openwisp-config/monitoring
	# size investigation below, still short even with this and owut/
	# attendedsysupgrade-common also out.
	#
	# wb-picwdt added 2026-09-15: this device has an external PIC watchdog
	# (see package/wb-picwdt) that resets the board ~30s after boot unless
	# fed -- discovered the hard way when a real unit's rootfs got
	# corrupted (unrelated incident, since fixed) and recovery required
	# booting a scratch initramfs kernel, which showed the REAL shipped
	# firmware had never included a watchdog feeder at all and was always
	# going to reset in a loop the moment boot got far enough to matter.
	# Without this, the device cannot stay up past the failsafe-mode
	# window on any boot, corrupted rootfs or not.
	#
	# wb-picwdt REMOVED again 2026-09-16 -- superseded by a real in-kernel
	# driver (drivers/watchdog/wb_picwdt.c, CONFIG_WB_PICWDT=y, registered
	# via the "picwdt" DT node), not dropped. The userspace daemon and the
	# kernel driver bit-bang the SAME two GPIO lines (P0.6/P0.7) -- running
	# both at once would corrupt the PIC protocol, so this is an either/or
	# swap, not an addition. Why: procd's sysupgrade handoff kills nearly
	# all userspace before the actual flash write, which would kill this
	# daemon mid-upgrade with nothing left feeding the PIC -- the exact
	# failure mode that corrupted this device once already (see
	# project_wattbox_wb300vb_recovery memory, 2026-09-14 incident). A
	# kthread started from the kernel driver's own probe() isn't a
	# process procd's cleanup can kill, so it survives that handoff with
	# no special handling -- making a normal `sysupgrade`/`owut upgrade`
	# safe on this device for the first time.
	#
	# openwisp-config/openwisp-monitoring DROPPED again 2026-09-16: live
	# rootfs measured 2,083,352 bytes with them in -- 313,880 bytes over
	# the 1,769,472 mtd2 budget, confirming (a second time, now on a real
	# build) the 2026-09-15 size investigation's conclusion. They do not
	# fit on this device at all, full stop -- not a config or trimming
	# problem, an actual capacity shortfall. Re-add only alongside a real
	# repartition or a smaller alternative to openwisp-config's own chain
	# (curl+lua+libuci-lua+luafilesystem+luci-lib-nixio+ca-certificates).
	DEVICE_PACKAGES := owut wbctl
endef
TARGET_DEVICES += snapav_wattbox-wb300vb

# NOT a real, flashable device -- exists purely to get openwisp-config/
# openwisp-monitoring compiled and published to the package feed for
# runtime `apk add` onto an already-running WattBox's overlay (see
# gotcha_openwrt_package_select_forces_install memory: on a single-profile
# target, CONFIG_PACKAGE_x=y always installs into that profile's own
# TARGET_DIR, so there is no way to select a package into the feed without
# also selecting it into SOME device's image -- CONFIG_TARGET_MULTI_PROFILE
# + CONFIG_TARGET_PER_DEVICE_ROOTFS makes that "some device" this one
# instead of the real snapav_wattbox-wb300vb profile). IMAGES left empty
# deliberately: this device never builds a kernel/rootfs/sysupgrade.bin at
# all, so it can never hit check-wattbox-rootfs-size or any other real
# device's build failure -- DEVICE_PACKAGES only needs to exist long enough
# for target-metadata.pl to emit `select DEFAULT_openwisp-config` /
# `select DEFAULT_openwisp-monitoring` against this profile's own Kconfig
# option, which is a source-scan-time effect, independent of whether any
# image ever gets built for it.
define Device/snapav_wattbox-wb300vb-openwisp-feed
	DEVICE_VENDOR := SnapAV
	DEVICE_MODEL := WattBox WB-300VB-IP-5 (openwisp feed carrier, not a real image)
	DEVICE_DTS := lpc3250-wattbox-wb300vb
	IMAGES :=
	DEVICE_PACKAGES := openwisp-config openwisp-monitoring
endef
TARGET_DEVICES += snapav_wattbox-wb300vb-openwisp-feed

$(eval $(call BuildImage))
