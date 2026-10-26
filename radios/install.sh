#!/bin/bash
# Installs the native radio services into the GNOME chroot and removes the
# older duplicate (android-connectivity extension + connectivity server).
# Run as root inside the desktop's mount namespace with the files staged in
# /data/local/tmp/radios-install (Android path, visible as /proc/1/root/...).
set -euo pipefail
SRC=/proc/1/root/data/local/tmp/radios-install
BACKUP=/opt/xscreen/backup/radios-$(date +%Y%m%d-%H%M%S)
EXT=/usr/share/gnome-shell/extensions/android-connectivity@ubuntu26.local
AGENT=/proc/1/root/data/local/ubuntu26/bridge/native-network-agent.jar

mkdir -p "$BACKUP"
for f in /usr/local/libexec/ubuntu26-native-network /usr/local/libexec/ubuntu26-connectivity-server \
         /usr/local/libexec/ubuntu26-connectivity-client /usr/local/sbin/ubuntu26-gnome \
         /etc/dbus-1/system.d/org.freedesktop.NetworkManager.conf "$AGENT"; do
    [ -e "$f" ] && cp -a "$f" "$BACKUP/"
done
[ -d "$EXT" ] && cp -a "$EXT" "$BACKUP/"

# Copy beside the target and rename over it, so running services keep a
# consistent file (uutils install also refuses to replace a file in use).
put() {
    cp "$1" "$2.new"
    chmod "$3" "$2.new"
    mv -f "$2.new" "$2"
}
put "$SRC/ubuntu26-native-network" /usr/local/libexec/ubuntu26-native-network 755
put "$SRC/ubuntu26-rfkill" /usr/local/libexec/ubuntu26-rfkill 755
put "$SRC/ubuntu26-radios.conf" /etc/dbus-1/system.d/ubuntu26-radios.conf 644
put "$SRC/ubuntu26-rfkill.desktop" /etc/xdg/autostart/ubuntu26-rfkill.desktop 644
put "$SRC/radio-agent.jar" "$AGENT" 400

# The extension duplicated GNOME's own Wi-Fi switch; Bluetooth is native now.
rm -rf "$EXT" /usr/local/libexec/ubuntu26-connectivity-server /usr/local/libexec/ubuntu26-connectivity-client
python3 - <<'EOF'
import pathlib
wrapper = pathlib.Path('/usr/local/sbin/ubuntu26-gnome')
text = wrapper.read_text()
for old in ('connectivity_pid=\n',
            '    [[ -z $connectivity_pid ]] || kill -TERM "$connectivity_pid" 2>/dev/null || true\n',
            '/usr/bin/python3 /usr/local/libexec/ubuntu26-connectivity-server \\\n'
            '    > /home/ubuntu/.local/state/anland/android-connectivity.log 2>&1 &\n'
            'connectivity_pid=$!\n\n'):
    text = text.replace(old, '', 1)
wrapper.write_text(text)
EOF
bash -n /usr/local/sbin/ubuntu26-gnome
echo "backup: $BACKUP"
