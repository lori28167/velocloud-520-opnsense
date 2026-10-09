#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Adds if_vcigb, the VeloCloud loader settings and LAN/WAN defaults to the
# OPNsense 26.7 serial image. Run as root with the image's own root as /,
# e.g. chroot into a read-write nullfs view of it (see README, "Install image").
# Expects if_vcigb.ko, if_vcigb.ko.sha256 and loader.conf.local next to it.
set -e
M=$(dirname "$0")
WANT_KERNEL="stable/26.7-n283674-12334a596709"
strings /boot/kernel/kernel | grep -q "$WANT_KERNEL" || { echo "PATCH: kernel is not $WANT_KERNEL, module would not match"; exit 1; }
[ "$(sha256 -q "$M/if_vcigb.ko")" = "$(cat "$M/if_vcigb.ko.sha256")" ] || { echo "PATCH: module checksum mismatch"; exit 1; }
install -d -m 755 /boot/modules
install -m 555 "$M/if_vcigb.ko" /boot/modules/if_vcigb.ko
kldxref /boot/modules
install -m 644 "$M/loader.conf.local" /boot/loader.conf.local
# Factory config: LAN = vcigb1 (LAN1-4), WAN = vcigb2 (GE1), only inside <interfaces>
C=/usr/local/etc/config.xml
awk '
/<interfaces>/ {inif = 1}
/<\/interfaces>/ {inif = 0}
inif && /<wan>/ {cur = "wan"}
inif && /<lan>/ {cur = "lan"}
inif && /<\/(wan|lan)>/ {cur = ""}
inif && cur == "wan" && /<if>/ {sub(/<if>[^<]*<\/if>/, "<if>vcigb2</if>")}
inif && cur == "lan" && /<if>/ {sub(/<if>[^<]*<\/if>/, "<if>vcigb1</if>")}
{print}' "$C" > "$C.new"
mv "$C.new" "$C"
chmod 644 "$C"
sync
echo "PATCH: modules"; ls -l /boot/modules
echo "PATCH: interfaces"; sed -n '/<interfaces>/,/<\/interfaces>/p' "$C" | grep -E '<(wan|lan|if)>'
echo "PATCH OK"
