define Device/checkpoint_nand
	DEVICE_VENDOR := Check Point
	# This U-Boot build (Check Point/Annapurna fork, 2016) has no
	# `bootelf` (confirmed live: "Unknown command 'bootelf'") — that
	# only works on MikroTik's own RouterBOOT, a different bootloader
	# entirely. `zImage`+`bootz` hit a real OpenWrt kernel-build.mk
	# quirk (unconditionally expects a nonexistent
	# arch/arm/boot/compressed/zImage for this target). Use a legacy
	# uImage + `bootm` instead — the exact mechanism the stock firmware
	# already uses successfully every boot on this hardware. DTB is
	# fetched separately (bootm's 3-arg external-FDT form), matching
	# how the stock firmware's own tf1_image1 script works.
	KERNEL_NAME := Image
	KERNEL_INITRAMFS := kernel-bin | uImage none
	KERNEL := kernel-bin | uImage none | package-kernel-ubifs | \
		ubinize-kernel
	# Stock kernel's own proven load address on this exact hardware
	# ("Load Address: 00008000" in every boot log this session) — RAM
	# base is 0x0 here (Zone ranges start at 0x0), so this is the
	# standard ARM TEXT_OFFSET convention, not a guess.
	KERNEL_LOADADDR := 0x00008000
	IMAGES := sysupgrade.bin
	IMAGE/sysupgrade.bin := sysupgrade-tar | append-metadata
endef

# NAND geometry measured live from the device (UBI attach log at boot):
# "UBI: PEB size: 262144 bytes (256 KiB) ... min./max. I/O unit sizes: 4096/4096"
# — different from the RB1100AHx4's 128k/2048, not a copy-paste guess.
define Device/checkpoint_l72w
	$(call Device/checkpoint_nand)
	DEVICE_MODEL := L-72W
	SOC := al31400
	BLOCKSIZE := 256k
	PAGESIZE := 4096
	KERNEL_UBIFS_OPTS = -m $$(PAGESIZE) -e 248KiB -c $$(PAGESIZE) -x none
	DEVICE_PACKAGES := yafut nand-utils
endef
TARGET_DEVICES += checkpoint_l72w
