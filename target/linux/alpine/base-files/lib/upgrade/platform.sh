REQUIRE_IMAGE_METADATA=1

RAMFS_COPY_BIN='fw_printenv fw_setenv'
RAMFS_COPY_DATA='/etc/fw_env.config /var/lock/fw_printenv.lock'

platform_do_upgrade_mikrotik_nand() {
	local fw_mtd=$(find_mtd_part kernel)
	fw_mtd="${fw_mtd/block/}"
	[ -n "$fw_mtd" ] || return

	local board_dir=$(tar tf "$1" | grep -m 1 '^sysupgrade-.*/$')
	board_dir=${board_dir%/}
	[ -n "$board_dir" ] || return

	local kernel_len=$(tar xf "$1" ${board_dir}/kernel -O | wc -c)
	[ -n "$kernel_len" ] || return

	tar xf "$1" ${board_dir}/kernel -O | ubiformat "$fw_mtd" -y -S $kernel_len -f -

	CI_KERNPART="none"
	nand_do_upgrade "$1"
}

# checkpoint_l72w's real MTD partition table (confirmed live every
# boot, see project_l72w_openwrt.md) has separate "linux_kernel" and
# "ubifs" partitions -- neither is named "kernel" or "ubi", the names
# nand.sh's own CI_KERNPART/CI_UBIPART default to. Same fundamental
# shape as the mikrotik board above (KERNEL := ... |
# package-kernel-ubifs | ubinize-kernel in checkpoint.mk means the
# sysupgrade tar's "kernel" entry is a FULL ubinize-kernel-produced
# UBI image, not a bare kernel binary or bare UBI-volume content --
# it needs a whole-partition `ubiformat`, not `ubiupdatevol` into an
# already-formatted volume) -- so this mirrors that function, pointed
# at our own partition names, plus CI_ROOT_UBIPART since our rootfs
# UBI partition isn't named "ubi" either.
#
# Deliberately only ever touches the PRIMARY slot (linux_kernel/
# ubifs) -- this device also has a second, redundant linux_kernel2/
# ubifs-2 pair (Check Point's own A/B failover scheme) that OpenWrt's
# sysupgrade has no concept of at all. Leaving that slot completely
# untouched keeps Check Point's own stock-firmware fallback intact as
# a real safety net, on top of the boot-menu's maintenance-mode/
# factory-reset recovery paths.
#
# FIRST REAL ATTEMPT FAILED (2026-09-13), root-caused and fixed --
# `nand_upgrade_prepare_ubi()` in nand.sh unconditionally attaches a
# UBI device for the "kern" role too (via CI_KERN_UBIPART, falling
# back to CI_UBIPART), even though CI_KERNPART="none" already tells
# nand_upgrade_tar() to skip WRITING a new kernel volume -- attaching
# is unconditional regardless. Since CI_KERN_UBIPART was never set,
# it fell back to CI_UBIPART, which nand_do_flash_file() had ALREADY
# silently reassigned from its own default "ubi" to "rootfs" (its own
# built-in fallback for exactly this "ubi partition doesn't exist"
# case -- not something this board's code did) -- so it tried to
# attach a UBI device on an MTD partition literally named "rootfs",
# which doesn't exist either, and the whole upgrade aborted right
# there before ever touching the real "ubifs" partition. Real
# evidence, not guessed: ubiformat on "linux_kernel" completed 100%
# (confirmed on real hardware), then the very next step failed with
# "cannot find ubi mtd partition rootfs". Fixed by also setting
# CI_KERN_UBIPART to the same real partition already-just-ubiformatted
# above, so the unconditional attach succeeds (nothing new gets
# written there -- kernel_length is empty so the ubimkvol block never
# runs -- it just needs a valid, attachable partition name).
#
# SECOND real-hardware attempt still failed the same way (2026-09-13)
# -- same root cause, one role further down: nand_upgrade_prepare_ubi()
# unconditionally attaches a THIRD UBI device too, for the
# "rootfs_data" role (needed because our rootfs is squashfs, not
# ubifs, so it gets a separate writable overlay volume), via
# CI_DATA_UBIPART falling back to the same already-hijacked
# CI_UBIPART="rootfs". Real evidence: this time ubi0 (linux_kernel)
# AND ubi1 (ubifs) both attached successfully (confirmed live dmesg,
# fixed by CI_KERN_UBIPART above), and it still failed with the exact
# same "cannot find ubi mtd partition rootfs" -- one role later than
# before. rootfs_data lives as a second volume in the SAME "ubifs"
# UBI partition as rootfs itself (the standard OpenWrt NAND layout),
# so CI_DATA_UBIPART is the same partition as CI_ROOT_UBIPART.
#
# THIRD real-hardware attempt failed differently (2026-09-14): both
# ubi0 (linux_kernel) and ubi1 (ubifs) attached cleanly this time
# (confirmed live dmesg), but then "ubimkvol: error!: UBI device does
# not have free logical eraseblocks" / "cannot create rootfs volume".
# Real cause: unlike "linux_kernel" (which THIS function explicitly
# ubiformats, above), "ubifs" was only ever ubiattach'ed, never
# reformatted -- it still holds Check Point's OWN original UBI
# volume(s), under Check Point's own name(s), occupying all 880 PEBs
# (confirmed live: "available 0 LEBs"). nand_upgrade_prepare_ubi()
# only reclaims space by removing a volume already named "rootfs"/
# "rootfs_data" (ubirmvol) -- since no such volume exists yet (this
# is a first-ever OpenWrt install here, not a repeat sysupgrade),
# nothing gets freed and there's no room for a new one. The mikrotik
# function above doesn't need this extra step because RB1100AHx4
# ships from the factory with that partition already blank/OpenWrt-
# formatted; a real Check Point production appliance's "ubifs"
# partition never is. Fixed by explicitly wiping it too, exactly like
# "linux_kernel" already is (this board's whole design already
# accepts sacrificing the primary slot's stock content in exchange
# for OpenWrt -- see the "Deliberately only ever touches the PRIMARY
# slot" note above -- so this isn't a new risk, just the same decision
# applied consistently to both partitions).
#
# FOURTH attempt: CONFIRMED WORKING on real hardware (2026-09-14).
# "sysupgrade successful" -- rootfs (3.6 MiB) and rootfs_data (169.2
# MiB) UBI volumes both created on "ubifs", kernel UBI volume written
# to "linux_kernel". Verified after the post-upgrade reboot (booted
# back to the TFTP test image, bootcmd untouched by design -- see
# below): re-attached both MTDs fresh, ubi0/ubi1 both show the
# correct volume layout, and the "rootfs" UBI block device mounts as
# a real, valid squashfs with /etc/openwrt_release correctly reading
# DISTRIB_TARGET='alpine/checkpoint'. This is real OpenWrt content
# sitting in this device's actual NAND, not a guess.
#
# That attempt-4 write above was real and confirmed, but turned out to
# be unbootable: read the real `tf1_image1` U-Boot script (the actual
# boot target for normal/debug/maintenance mode -- all three share the
# same routine) via `fw_printenv` and confirmed, then verified live by
# setting bootcmd to it and rebooting, that this bootloader does a RAW
# NAND READ of "linux_kernel" straight into RAM and `bootm`s starting
# 0x1000 bytes in -- it expects a plain legacy uImage with a leading
# 4KB header (Check Point's own metadata block: version strings, a
# CRC, no relation to boot), NOT a UBI container. Confirmed via a real
# boot log on the STOCK kernel too (2026-09-14, maintenance mode):
# "NAND read: device 0 offset 0x380000, size 0x1000000" (raw read of
# the whole 16MB) then "## Booting kernel from Legacy Image at
# 08001000" (0x1000 past loadaddr) -- the leading 4KB is never read or
# validated by this path at all, it's dead weight for booting.
# UBI(UBIFS(uImage))-wrapping "linux_kernel" (what checkpoint.mk did
# through attempt 4) broke every stock boot path simultaneously, not
# just NAND-boot testing -- recovered via boot-menu option 4 (factory
# reset), which also reverted "ubifs" back to Check Point's stock
# content, undoing this sysupgrade write entirely. See
# project_l72w_openwrt.md for the full recovery.
#
# FIX: checkpoint.mk's KERNEL pipeline now produces a plain
# 4096-zero-byte-header + legacy uImage (matches the real on-flash
# shape, just with a blank/unused header instead of Check Point's own
# metadata -- confirmed unused by the boot path above, so zero is
# fine) instead of a UBI container. "linux_kernel" is therefore a
# genuinely raw MTD partition now, not UBI -- which nand.sh's own
# *generic*, unmodified `nand_upgrade_tar()` already fully supports:
# when `CI_KERNPART` names a real MTD partition (not "none"), it does
# a plain `mtd write` of the kernel (with the standard "zero the first
# 4KB first" invalidate-before-write safety step -- harmless here
# since that's exactly our blank header region anyway) instead of any
# UBI volume logic. So the whole kernel side of this function goes
# away -- no more manual ubiformat/tar-extract/kernel_len plumbing.
#
# `CI_KERN_UBIPART` still needs to be set to something attachable as
# UBI, because `nand_upgrade_prepare_ubi()` unconditionally attaches a
# UBI device for the "kern" role regardless of whether anything is
# being written there (same underlying nand.sh behavior as attempts
# 1-3 above) -- but it must NOT be "linux_kernel" anymore now that
# that partition is raw MTD: attaching a non-UBI partition as UBI
# fails, and `nand_attach_ubi()`'s own fallback on failed attach is to
# `ubiformat` it, which would destroy the raw kernel image we just
# wrote. Pointed at "ubifs" instead (already real UBI, already
# attached for the root/data roles) -- the lookup for a volume named
# "linux_kernel" within it simply finds nothing and no-ops, exactly
# like the existing "data" role already does when nothing new is
# being written there.
#
# Still NOT done: bootcmd is still pointed at the TFTP test sequence
# (deliberately, throughout this whole project) -- flipping it to
# `run tf1_image1` is the real next test, and this time doesn't need
# to happen before validating the image format: boot-menu option 3
# (maintenance mode) uses the identical raw-read+bootm routine and can
# be used to test the new kernel format directly, without ever
# touching bootcmd -- so the natural-autoboot-timeout TFTP fallback
# stays intact as a real safety net regardless of the outcome.
platform_do_upgrade_checkpoint_l72w() {
	local root_mtd=$(find_mtd_part ubifs)
	root_mtd="${root_mtd/block/}"
	[ -n "$root_mtd" ] || return

	# Check Point's own stock UBI volume ("pfrm2.0") still occupies
	# every PEB on a first-ever install -- see attempt 3 above.
	#
	# Real follow-on bug (2026-09-14): once `ubi.mtd=ubifs` was added
	# to CONFIG_CMDLINE (see checkpoint/config-default, needed so the
	# kernel can find "rootfs" at boot at all), the kernel auto-
	# attaches ubi0 on this partition on EVERY boot -- so `ubiformat`
	# below always fails with "please, first detach mtd3 from ubi0"
	# unless explicitly detached first. Only didn't break the actual
	# 2026-09-14 test run because our own "rootfs"/"rootfs_data"
	# volumes from an earlier sysupgrade were already present, so
	# nand_do_upgrade's own generic name-matching reclaim handled it
	# without ever needing this explicit ubiformat to succeed -- a
	# genuine first-ever/factory-reset install (real "pfrm2.0" stock
	# volume, not named "rootfs") would still fail outright without
	# this detach.
	nand_detach_ubi ubifs
	ubiformat "$root_mtd" -y

	# Flash the external device tree to its own MTD partition. This board boots
	# with an external FDT (bootm 3-arg): the bootcmd raw-reads the kernel from
	# "linux_kernel" and the DTB from "device_tree", so the DTB has to live in
	# flash. checkpoint.mk bundles it into the sysupgrade tar as
	# ${board_dir}/dtb; without this write, a NAND boot falls back to the stock
	# bootloader's generic TOC DTB (Annapurna "Alpine Dev Board" -- no prestera
	# switch, 0 LAN ports), which is exactly the symptom seen before this.
	#
	# "device_tree" (mtd1, NAND 0x200000-0x300000) physically aliases the top of
	# al_boot, including the redundant U-Boot env at its own 0x80000/0xc0000
	# (= NAND 0x280000/0x2c0000). That's safe here: `mtd write` erases lazily,
	# one eraseblock at a time only as the write cursor reaches it
	# (package/system/mtd/src/mtd.c mtd_write) -- a ~10KB DTB touches exactly
	# block 0 (NAND 0x200000-0x240000, all-0xff/unused) and never reaches the
	# env. Skipped cleanly for images built before DTB bundling.
	local board_dir=$(tar tf "$1" | grep -m 1 '^sysupgrade-.*/$')
	board_dir=${board_dir%/}
	if [ -n "$board_dir" ] && tar tf "$1" | grep -q "^${board_dir}/dtb$"; then
		echo "Writing device tree to \"device_tree\" partition"
		tar xf "$1" ${board_dir}/dtb -O | mtd write - device_tree
	fi

	CI_KERNPART="linux_kernel"
	CI_KERN_UBIPART="ubifs"
	CI_ROOT_UBIPART="ubifs"
	CI_DATA_UBIPART="ubifs"
	nand_do_upgrade "$1"
}

platform_check_image() {
	return 0
}

platform_do_upgrade() {
	case "$(board_name)" in
	mikrotik,rb1100ahx4)
		platform_do_upgrade_mikrotik_nand "$1"
		;;
	checkpoint,l72w)
		platform_do_upgrade_checkpoint_l72w "$1"
		;;
	*)
		default_do_upgrade "$1"
		;;
	esac
}
