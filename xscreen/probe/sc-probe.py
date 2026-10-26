#!/usr/bin/python3
"""Create a mutter virtual monitor via org.gnome.Mutter.ScreenCast and
report how its PipeWire stream negotiates buffers (DmaBuf vs MemFd)."""
import subprocess, sys, os
from gi.repository import Gio, GLib

mode = sys.argv[1] if len(sys.argv) > 1 else 'dmabuf'
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
SC = 'org.gnome.Mutter.ScreenCast'
session_path = bus.call_sync(SC, '/org/gnome/Mutter/ScreenCast', SC, 'CreateSession',
    GLib.Variant('(a{sv})', ({},)), GLib.VariantType('(o)'), 0, -1, None).unpack()[0]
stream_path = bus.call_sync(SC, session_path, SC + '.Session', 'RecordVirtual',
    GLib.Variant('(a{sv})', ({'cursor-mode': GLib.Variant('u', 2),
                              'is-platform': GLib.Variant('b', True)},)),
    GLib.VariantType('(o)'), 0, -1, None).unpack()[0]
loop = GLib.MainLoop()
node = {}
def on_signal(conn, sender, path, iface, name, params, *a):
    node['id'] = params.unpack()[0]; loop.quit()
bus.signal_subscribe(SC, SC + '.Stream', 'PipeWireStreamAdded', stream_path, None, 0, on_signal)
bus.call_sync(SC, session_path, SC + '.Session', 'Start', None, None, 0, -1, None)
GLib.timeout_add_seconds(10, loop.quit); loop.run()
print('node', node.get('id'), flush=True)
if mode == 'enum':
    import time; time.sleep(0.5)
    r = subprocess.run(['pw-cli','enum-params',str(node.get('id')),'EnumFormat'],capture_output=True,text=True)
    print(r.stdout[-6000:], r.stderr[-500:])
    bus.call_sync(SC, session_path, SC + '.Session', 'Stop', None, None, 0, -1, None); sys.exit(0)
caps = {'dmabuf': 'video/x-raw(memory:DMABuf),format=DMA_DRM,width=1920,height=1080,framerate=60/1',
        'memfd': 'video/x-raw,width=1920,height=1080,framerate=60/1'}[mode]
env = dict(os.environ, GST_DEBUG='pipewiresrc:4,pipewire:3', PIPEWIRE_DEBUG='2')
p = subprocess.run(['timeout', '12', 'gst-launch-1.0', '-v', 'pipewiresrc', f'path={node.get("id")}',
                    'do-timestamp=true', 'num-buffers=30', '!', caps, '!', 'fakesink', 'sync=false'],
                   env=env, capture_output=True, text=True)
out = p.stdout + p.stderr
for l in out.splitlines():
    if any(k in l for k in ('caps =', 'DmaBuf', 'MemFd', 'dmabuf', 'ERROR', 'error', 'negotiat', 'EOS', 'Got EOS', 'modifier', 'drm-format')):
        print(l[:300])
print('rc', p.returncode)
bus.call_sync(SC, session_path, SC + '.Session', 'Stop', None, None, 0, -1, None)
