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
	# Real, hardware-confirmed on-flash format (2026-09-14, see
	# project_l72w_openwrt.md): the stock bootloader's own
	# `tf1_image1` script (shared by normal/debug/maintenance boot)
	# does a raw NAND read of the whole "linux_kernel" partition into
	# RAM, then `bootm`s starting 4096 bytes in — i.e. a plain legacy
	# uImage with a leading 4KB header. Confirmed via a real boot log
	# on the stock kernel: "NAND read: ... size 0x1000000" (the raw
	# read) then "## Booting kernel from Legacy Image at 08001000"
	# (loadaddr+0x1000). The header itself is Check Point's own
	# metadata (version strings, a CRC) for some other tool — never
	# read or validated by this boot path — so a blank/zero header
	# works identically. An earlier attempt wrapped this in
	# UBI(UBIFS(uImage)) instead (matching the mikrotik NAND target
	# this was copied from) — that let sysupgrade WRITE successfully,
	# but broke every stock boot path simultaneously the moment
	# "linux_kernel" held UBI content instead of a raw uImage
	# ("Wrong Image Format for bootm command", confirmed live).
	KERNEL := kernel-bin | uImage none | checkpoint-kernel-header
	# Stock kernel's own proven load address on this exact hardware
	# ("Load Address: 00008000" in every boot log this session) — RAM
	# base is 0x0 here (Zone ranges start at 0x0), so this is the
	# standard ARM TEXT_OFFSET convention, not a guess.
	KERNEL_LOADADDR := 0x00008000
	IMAGES := sysupgrade.bin
	# Bundle the board DTB into the sysupgrade tar (as sysupgrade-<board>/dtb).
	# This board boots with an EXTERNAL FDT (bootm's 3-arg form) read from a
	# dedicated flash partition -- so the DTB has to be flashed alongside the
	# kernel/rootfs, otherwise a NAND boot falls back to the stock bootloader's
	# generic TOC DTB (Annapurna "Alpine Dev Board": no prestera switch, 0 LAN
	# ports). platform_do_upgrade_checkpoint_l72w writes this entry to the
	# "device_tree" MTD partition. Path/escaping mirror the canonical in-define
	# form used by mvebu/siflower (sysupgrade-tar dtb=...).
	IMAGE/sysupgrade.bin := sysupgrade-tar dtb=$$(KDIR)/image-$$(firstword $$(DEVICE_DTS)).dtb | append-metadata
endef

define Build/checkpoint-kernel-header
	( dd if=/dev/zero bs=4096 count=1 2>/dev/null; cat $@ ) > $@.new
	mv $@.new $@
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
	# 3 real PCI endpoints behind pcie_external0/1/2 in alpine-l72w.dts:
	# 168c:0033 (Qualcomm/Atheros AR9580, ath9k -- no external firmware
	# needed, calibration is on-chip EEPROM), 168c:003c (Qualcomm/
	# Atheros QCA988x, ath10k -- needs external firmware + board data;
	# using the community/generic qca988x board file since Check
	# Point's OEM calibration isn't available through any other means,
	# same reasoning as leaving eth3's MAC address unset), and
	# 1912:0014 (Renesas xHCI USB3, kmod-usb3 + kmod-usb-xhci-pci-
	# renesas) -- an explicit axi_slave_err_resp clear (see
	# pcie_external2's own note in alpine-l72w.dts) fixed the kernel
	# panic, but it still doesn't functionally work ("Host halt
	# failed, -19"); tabled, see reference_bored_list.md (2026-09-13).
	#
	# 2 more PCI endpoints on the INTERNAL pcie bus (same bus as the 4
	# al_eth MACs, no DT node needed): al_crypto (1c36:0011) and al_dma
	# (1c36:0021), Annapurna Labs' own crypto/DMA offload engines.
	# Fully ported drivers already existed in files-6.18/drivers/
	# {crypto,dma}/al/ from a prior session; just never enabled. Built
	# into the kernel via checkpoint/config-default (no kmod-* package
	# exists for either) -- CONFIG_CRYPTO_DEV_AL_CRYPTO/CONFIG_AL_DMA.
	#
	# Both CONFIRMED WORKING on real hardware (2026-09-13). al_dma
	# probes both PCI functions (0000:00:05.0/.1), "al_dma: Annapurna
	# Labs DMA Driver 0.01" in dmesg. al_crypto (0000:00:04.0,
	# 1c36:0011) probes too -- full skcipher/aead/hash/crc algorithm
	# registration in /proc/crypto (one benign failure:
	# "hmac-sha256-al alg registration failed with -17", a name
	# collision with the generic software hmac(sha256), doesn't block
	# anything else). An earlier same-session check said al_crypto
	# never ran at all -- that was a false negative from checking
	# dmesg/sysfs too soon after issuing `reboot` over the serial
	# harness, racing the actual power cycle. See project_l72w_openwrt.md.
	#
	# i2c0 (0xfd880000, alpine.dtsi) carries the board's LED-driving
	# GPIO expanders (pcf8575 @ 0x20/0x21 -- real per-port/per-speed
	# LEDs, see alpine-l72w.dts and project_l72w_openwrt.md for the
	# full live-confirmed hardware map). kmod-leds-gpio is already in
	# the shared alpine Makefile's DEFAULT_PACKAGES; this adds the I2C
	# controller driver + the GPIO expander driver. Only the DMZ-
	# copper LEDs (eth1) have a wired netdev trigger so far -- not yet
	# boot-tested on real hardware (2026-09-13).
	DEVICE_PACKAGES := yafut nand-utils \
		kmod-usb3 kmod-usb-xhci-pci-renesas \
		kmod-ath9k \
		kmod-ath10k ath10k-firmware-qca988x-ct \
		kmod-i2c-designware-platform kmod-gpio-pcf857x \
		owut luci-app-attendedsysupgrade
endef
TARGET_DEVICES += checkpoint_l72w
