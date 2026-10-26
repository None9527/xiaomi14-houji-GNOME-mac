#!/usr/bin/python3
"""set-mode.py WxH[@RATE] [SCALE]: switch the XScreen monitor's mode through
org.gnome.Mutter.DisplayConfig, the way GNOME Settings does (temporary)."""
import sys
from gi.repository import Gio, GLib

want, scale = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
DC = 'org.gnome.Mutter.DisplayConfig'
serial, monitors, logical, props = bus.call_sync(DC, '/org/gnome/Mutter/DisplayConfig', DC,
    'GetCurrentState', None, None, 0, -1, None).unpack()
modes = {}
for (conn, vendor, product, mserial), mlist, mprops in monitors:
    cur = next(m[0] for m in mlist if m[6].get('is-current'))
    pick = cur
    if vendor == 'XScreen':
        pick = next(m[0] for m in mlist if m[0].startswith(want))
    modes[conn] = pick
configs = []
for x, y, sc, tr, primary, specs, lprops in logical:
    conn = specs[0][0]
    vendor = specs[0][1]
    configs.append((x, y, scale if vendor == 'XScreen' else sc, tr, primary,
                    [(conn, modes[conn], {})]))
bus.call_sync(DC, '/org/gnome/Mutter/DisplayConfig', DC, 'ApplyMonitorsConfig',
    GLib.Variant('(uua(iiduba(ssa{sv}))a{sv})', (serial, 1, configs, {})), None, 0, -1, None)
print('applied', {k: v for k, v in modes.items()})
