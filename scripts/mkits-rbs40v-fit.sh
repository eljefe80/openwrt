#!/bin/sh
#
# Licensed under the terms of the GNU GPL License version 2 or later.
#
# Author: Peter Tyser <ptyser@xes-inc.com>
#
# U-Boot firmware supports the booting of images in the Flattened Image
# Tree (FIT) format.  The FIT format uses a device tree structure to
# describe a kernel image, device tree blob, ramdisk, etc.  This script
# creates an Image Tree Source (.its file) which can be passed to the
# 'mkimage' utility to generate an Image Tree Blob (.itb file).  The .itb
# file can then be booted by U-Boot (or other bootloaders which support
# FIT images).  See doc/uImage.FIT/howto.txt in U-Boot source code for
# additional information on FIT images.
#

usage() {
        echo "Usage: `basename $0` output config@<ident>"
        exit 1
}

# We need at least 3 arguments
[ "$#" -lt 2 ] && usage

OUTPUT=${1}
CONFIG=${2}

ARCH_UPPER=$(echo "$ARCH" | tr '[:lower:]' '[:upper:]')

BOOTSCRIPT="
if test \"\$machid\" = \"8010006\"; then\n
 nand device 0 && imxtract \$imgaddr kernel-1 &&
 nand erase 0x02580000 0x06980000 &&
 nand write \$fileaddr 0x02580000 0x03700000 &&
exit 0\n
fi\n
exit 1\n
"

echo ${BOOTSCRIPT} > ${OUTPUT}.scr

# Create a default, fully populated DTS file
DATA="/dts-v1/;

/ {
	description = \"${ARCH_UPPER} OpenWrt FIT (Flattened Image Tree)\";
	#address-cells = <1>;

	images {
                script {
                        description = \"Flashing nand 800 20000\";
                        type = \"script\";
                        data = /incbin/(\"${OUTPUT}.scr\");
                };
		kernel-1 {
			description = \"${ARCH_UPPER} OpenWrt Linux\";
			data = /incbin/(\"${OUTPUT}\");
			type = \"kernel\";
			arch = \"${ARCH}\";
			os = \"linux\";
			compression = \"none\";
			load = <0x200000>;
			entry = <0x200000>;
			hash-1 {
				algo = \"crc32\";
			};
		};
	};

	configurations {
		default = \"${CONFIG}\";
		${CONFIG} {
			description = \"OpenWrt ${DEVICE}\";
			kernel = \"kernel-1\";
		};
	};
};"

# Write .its file to disk
echo "$DATA" > "${OUTPUT}".its
