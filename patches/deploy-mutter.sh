#!/bin/bash
# Install the locally built mutter packages on the phone and restart GNOME.
# Rollback: reinstall Anland's stock mutter debs (50.1-0ubuntu91).
set -eu
R=/data/local/ubuntu26/rootfs
NS_EXEC="/data/adb/ksu/bin/busybox chroot $R /usr/bin/env PATH=/usr/sbin:/usr/bin:/sbin:/bin"
adb shell "su -c '$NS_EXEC /bin/sh -c \"cd /opt/xscreen/build && ls *.deb && dpkg -i libmutter-18-0_*xs1*_arm64.deb mutter-common_*xs1*_all.deb mutter-common-bin_*xs1*_arm64.deb gir1.2-mutter-18_*xs1*_arm64.deb mutter_*xs1*_arm64.deb 2>&1 | tail -4\"'"
adb shell "su -M -c '/data/local/ubuntu26/ubuntu26-service gnome stop; sleep 1; /data/local/ubuntu26/ubuntu26-service gnome start'"
adb shell "am start -n com.anland.termux/.MainActivity >/dev/null"
for i in $(seq 1 40); do
  adb shell 'pidof gnome-shell' >/dev/null 2>&1 && break; sleep 1
done
echo "gnome-shell: $(adb shell pidof gnome-shell)"
