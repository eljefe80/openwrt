# NETGEAR RBS40V (Orbi Voice) board support

This file documents this branch's RBS40V-specific structure. `ipq40xx` is a
shared target hosting many devices; nothing here applies to any board but
`netgear_rbs40v`.

## What lives in this branch (kernel/OpenWrt bring-up)

Everything needed to get this specific hardware booting and working under
OpenWrt: devicetree (`dts/qcom-ipq4019-rbs40v.dts`), kernel config
(`config-6.18`), the image recipe (`image/generic.mk`), kernel module
patches (`package/kernel/ipq40xx-snd`, `package/kernel/rbs40v-touchpad`),
the `rbs40v-inittab` package (frees the Bluetooth UART from a console
getty -- a build-time fix, not RBS40V application logic), the
`bccmd` package (a generic, non-board-specific BlueZ 4.101 tool bluez5
dropped -- kept in-tree because it's a missing-upstream-package, not
RBS40V-specific integration code), and the Bluetooth bring-up script +
pskey blob (`base-files/etc/init.d/rbs40v-bluetooth`,
`base-files/etc/bluetooth/pb_207_csr8x11.psr`) -- tightly coupled to this
board's exact GPIO/UART wiring, not generically reusable.

## What's out-of-tree (its own package repo)

`led-ring-mqtt` (LP5562 RGBW ring <-> Home Assistant MQTT bridge) lives at
<https://github.com/eljefe80/led-ring-mqtt> -- an application-level
integration with no kernel/board dependency beyond standard LED-classdev
sysfs paths. Clone it in before building if you want it in the image:

```sh
git clone https://github.com/eljefe80/led-ring-mqtt.git package/led-ring-mqtt
make menuconfig      # Utilities -> led-ring-mqtt (M or *)
```

## What's out-of-tree entirely (not a package)

Mesh/fleet network setup (802.11s backhaul, VXLAN trunk, dawn steering)
lives at `~/nanoclaw/rbs40v-mesh/` on the build host, not in this repo at
all -- same convention as the WHW03 fleet's own mesh setup
(`~/nanoclaw/whw03-mesh/`). It's fleet deployment config, not board
bring-up: real secrets never touch this tree, and `set-secrets.sh` there
is run manually on the node after flashing.

## Build

```sh
# select target 'ipq40xx' / subtarget 'generic' / device 'netgear_rbs40v'
make -j"$(nproc)"
```

## First install

Stock U-Boot listens for NMRP (NETGEAR's own recovery protocol) briefly on
every boot. Flash `factory.chk` via `nmrpflash` (not `sysupgrade` -- this
board's stock partition layout doesn't have room for a rebuilt kernel, and
`platform_do_upgrade_netgear_orbi_upgrade` needs a `kernel=` file in the
sysupgrade tar that the shared `netgear_orbi` image macro doesn't provide):

```sh
sudo nmrpflash -i <interface-connected-to-the-board> -f bin/targets/ipq40xx/generic/openwrt-ipq40xx-generic-netgear_rbs40v-squashfs-factory.chk -v
```

Start `nmrpflash` first, then power-cycle (or reset from U-Boot) the board
to catch its boot-time NMRP listen window.
