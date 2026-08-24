#
# WattBox WB-300VB-IP-5 sysupgrade.
#
# The board boots a raw zImage+DTB from the "kernel" NOR partition (U-Boot
# cp.b+go), with a separate squashfs "rootfs" partition and a jffs2
# "rootfs_data" overlay on a DIFFERENT chip (SPI-NOR). Upgrade touches only the
# kernel + rootfs partitions, so the overlay -- and therefore /etc config and
# the wbctld outlet state -- survives a firmware upgrade.
#
# The sysupgrade image is [ kernel, padded to 128k ][ squashfs rootfs ].
# Split it at the squashfs magic and write each half to its own mtd.
#

# Echo the byte offset of the squashfs magic ('hsqs'); it is padded to a 128k
# boundary in the image. Empty + non-zero return if not found.
wb_squashfs_offset() {
	local img="$1" sz o=0
	sz=$(wc -c < "$img")
	while [ "$o" -lt "$sz" ]; do
		[ "$(dd if="$img" bs=4 skip=$((o / 4)) count=1 2>/dev/null)" = "hsqs" ] && {
			echo "$o"
			return 0
		}
		o=$((o + 131072))
	done
	return 1
}

platform_check_image() {
	local img="$1" off
	off=$(wb_squashfs_offset "$img") || {
		echo "Invalid image: no squashfs found"
		return 1
	}
	[ "$off" -ge 131072 ] || {
		echo "Invalid image: kernel region too small ($off)"
		return 1
	}
	return 0
}

platform_do_upgrade() {
	local img="$1" off kb rb
	off=$(wb_squashfs_offset "$img")   # 128k-aligned, so also 64k-aligned
	kb=$((off / 65536))
	# "rootfs" partition size (hex bytes from /proc/mtd) in 64k blocks; the
	# image's rootfs part is padded larger than the partition, so cap the write.
	rb=$(sed -n 's/^mtd[0-9]*: \([0-9a-f]*\) .*"rootfs".*/\1/p' /proc/mtd | head -1)
	rb=$(( (0x$rb) / 65536 ))
	# kernel = [0, off) -> "kernel" mtd; rootfs = [off, off+partition) -> "rootfs".
	dd if="$img" bs=65536 count="$kb" 2>/dev/null | mtd write - kernel
	dd if="$img" bs=65536 skip="$kb" count="$rb" 2>/dev/null | mtd write - rootfs
}
