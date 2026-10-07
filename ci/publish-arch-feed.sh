#!/bin/bash
# Merge one build's arch-level package feeds into the shared downloads tree.
#
# Several targets can share an arch (DL-WRX36 and Gemtek are both
# aarch64_cortex-a53), and every build only compiles the packages its own
# devices select. Replacing the arch feed on each publish therefore deleted
# the other target's packages (2026-10-06: Gemtek's publish removed
# DL-WRX36's ath11k-firmware/e2fsprogs/losetup). Instead each build records
# what it contributed in <dst>/.contrib/<id>; the feed is the union of all
# contributions, a package shipped by several builds comes from the newest
# one, and files nobody contributes any more are removed. The indexes are
# rebuilt the same way package/Makefile builds them, signed with the build's
# private-key.pem -- the fleet's persistent key, so every image and
# ImageBuilder already trusts it.
#
# usage: publish-arch-feed.sh <bin/packages/ARCH> <dst arch dir> <contributor id> <openwrt topdir>
set -euo pipefail

SRC=$(realpath "$1") DST=$2 WHO=$3 TOP=$(realpath "$4")
APK=$TOP/staging_dir/host/bin/apk
KEY=$TOP/private-key.pem
MKJSON=$TOP/scripts/make-index-json.py
ARCH=$(basename "$SRC")
[ -x "$APK" ] && [ -f "$KEY" ] && [ -f "$MKJSON" ] || { echo "missing apk/key/make-index-json under $TOP" >&2; exit 1; }

mkdir -p "$DST/.contrib"
exec 9>"$DST/.lock"
flock 9

# 1. This build's contribution, one "feed name version" line per package.
contrib=$(mktemp)
for d in "$SRC"/*/; do
	feed=$(basename "$d")
	[ -f "$d/packages.adb" ] || continue
	"$APK" adbdump --format json "$d/packages.adb" | python3 -c '
import json, sys
for p in json.load(sys.stdin)["packages"]:
    print(sys.argv[1], p["name"], p["version"])' "$feed"
	mkdir -p "$DST/$feed"
	cp -p "$d"/*.apk "$DST/$feed/"
done >"$contrib"
mv "$contrib" "$DST/.contrib/$WHO"

# 2. Union of every contribution; for a name shipped by several builds the
#    most recently published manifest wins.
wanted=$(mktemp)
python3 - "$DST/.contrib" >"$wanted" <<'EOF'
import os, sys
d = sys.argv[1]
pick = {}
for m in sorted(os.listdir(d), key=lambda f: os.path.getmtime(os.path.join(d, f))):
    for line in open(os.path.join(d, m)):
        feed, name, version = line.split()
        pick[(feed, name)] = version
for (feed, name), version in sorted(pick.items()):
    print(feed, f"{name}-{version}.apk")
EOF

# 3. Per feed: drop files nobody wants, rebuild and sign the indexes.
for d in "$DST"/*/; do
	feed=$(basename "$d")
	keep=$(awk -v f="$feed" '$1 == f { print $2 }' "$wanted")
	for apk in "$d"*.apk; do
		[ -e "$apk" ] || continue
		grep -qxF "$(basename "$apk")" <<<"$keep" || rm -f -- "${apk:?}"
	done
	if [ -z "$keep" ]; then
		rm -f -- "${d:?}packages.adb" "${d:?}index.json"
		continue
	fi
	(
		cd "$d"
		# shellcheck disable=SC2086 # one file name per line, no spaces
		"$APK" mkndx --root "$TOP" --keys-dir "$TOP" --allow-untrusted \
			--sign "$KEY" --output packages.adb $keep
		"$APK" adbdump --format json packages.adb |
			"$MKJSON" -f apk -a "$ARCH" - >index.json
	)
done
rm -f -- "${wanted:?}"

# 4. ASU's json_v1_arch_index enumerates feeds from this file.
for d in "$DST"/*/; do
	[ -f "$d/index.json" ] && echo "src-git $(basename "$d") generated"
done >"$DST/feeds.conf"
echo "Merged $WHO into $DST:"
cat "$DST/feeds.conf"
for m in "$DST"/.contrib/*; do echo "  $(basename "$m"): $(wc -l <"$m") packages"; done
