#!/bin/sh
# Fleet defaults for every eljefe80/openwrt ASU build. Each build-*.yml
# installs this as target/linux/generic/base-files/etc/uci-defaults/
# 97-fleet-defaults, so it ships in base-files and therefore in ASU-built
# (ImageBuilder) images too, on every target.
#
# uci-defaults run on the first boot after every flash, so this only fills
# in what is missing: per-device edits and openwisp's uuid/key survive
# upgrades.
#
# The opensoho shared secret is deliberately NOT here -- the ImageBuilder
# images are public. Add it per device; see opensoho-asu-stack/ONBOARDING.md.

ASU_URL='http://asu.int.wdwconsulting.net'
OPENSOHO_URL='http://opensoho.int.wdwconsulting.net'
REBIND_DOMAIN='int.wdwconsulting.net'
OWUT_CRON='0 4 * * * /usr/bin/owut check -q 2>&1 | logger -t owut'

# owut / attended sysupgrade: our ASU, not sysupgrade.openwrt.org.
if [ -x /usr/bin/owut ] || [ -f /etc/config/attendedsysupgrade ]; then
	touch /etc/config/attendedsysupgrade
	case "$(uci -q get attendedsysupgrade.server.url)" in
	''|*sysupgrade.openwrt.org*)
		uci -q batch <<-EOF
			set attendedsysupgrade.server=server
			set attendedsysupgrade.server.url='$ASU_URL'
			commit attendedsysupgrade
		EOF
		;;
	esac
fi

# Nightly read-only owut check (never builds or flashes on its own).
if [ -x /usr/bin/owut ] && ! grep -qs 'owut check' /etc/crontabs/root; then
	echo "$OWUT_CRON" >> /etc/crontabs/root
	/etc/init.d/cron enable 2>/dev/null
fi

# opensoho controller URL. The shared secret is added by hand.
if [ -f /etc/config/openwisp ] && [ -z "$(uci -q get openwisp.http.url)" ]; then
	uci -q batch <<-EOF
		set openwisp.http.url='$OPENSOHO_URL'
		commit openwisp
	EOF
fi

# opensoho/asu resolve to RFC1918 addresses; stock dnsmasq rebind
# protection would refuse them.
if uci -q get dhcp.@dnsmasq[0] >/dev/null &&
   ! uci -q get dhcp.@dnsmasq[0].rebind_domain | grep -qw "$REBIND_DOMAIN"; then
	uci -q batch <<-EOF
		add_list dhcp.@dnsmasq[0].rebind_domain='$REBIND_DOMAIN'
		commit dhcp
	EOF
fi

exit 0
