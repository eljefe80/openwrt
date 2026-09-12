define Device/checkpoint_nand
	DEVICE_VENDOR := Check Point
	KERNEL_NAME := vmlinux
	KERNEL_INITRAMFS := kernel-bin | append-dtb-elf
	KERNEL := kernel-bin | append-dtb-elf | package-kernel-ubifs | \
		ubinize-kernel
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
