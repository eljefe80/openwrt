# lpc32xx target — SnapAV WattBox WB-300VB-IP-5

Mainline-based OpenWrt target for the WattBox WB-300VB-IP-5 (NXP LPC3250 network
PDU): squashfs root on the parallel NOR, jffs2 overlay on the SPI NOR.

## The `wbctl` package is out-of-tree

The userspace control daemon (`wbctld` — outlets/LEDs/metering → MQTT + Home
Assistant) is maintained as its **own package repo**, not carried in this tree.
Clone it in before building if you want it in the image:

```sh
git clone https://github.com/eljefe80/wbctl.git package/wbctl
make menuconfig      # Utilities -> wbctl (M or *)
```

## Build

```sh
# select target 'lpc32xx' / device 'snapav_wattbox-wb300vb'
make -j"$(nproc)"
```

## Bootloader & first install

The stock U-Boot cannot cold-boot this image (it uses a hardcoded `bootm`; the
kernel is a raw `zImage`+DTB needing `go`), so a first install replaces U-Boot and
flashes the NOR over serial + TFTP. See:

- U-Boot patches, DRAM/PLL splice, flashing tools: <https://github.com/eljefe80/wattbox-wb300vb-uboot>
- Full stock → OpenWrt walkthrough: <https://gist.github.com/eljefe80/32540b67da96245256f7bc468c1a9b7f>
