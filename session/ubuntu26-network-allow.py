#!/usr/bin/python3
"""Allow only Ubuntu UID 60000 through Android's background network policy."""
import ctypes as C
import errno
import os
import platform
import struct
import sys

assert os.geteuid() == 0
assert platform.machine() in ('aarch64', 'arm64')
libc = C.CDLL(None, use_errno=True)

def report(message):
    if '--quiet' not in sys.argv:
        print(message)

def bpf(command, attr):
    buf = C.create_string_buffer(attr)
    result = libc.syscall(280, command, buf, len(attr))
    if result < 0:
        code = C.get_errno()
        raise OSError(code, os.strerror(code))
    return result

path = C.create_string_buffer(b'/proc/1/root/sys/fs/bpf/netd_shared/map_netd_uid_owner_map')
fd = bpf(7, struct.pack('QII', C.addressof(path), 0, 0))
try:
    info = C.create_string_buffer(256)
    bpf(15, struct.pack('IIQ', fd, 256, C.addressof(info)))
    _, _, key_size, value_size = struct.unpack_from('4I', info.raw)
    assert (key_size, value_size) == (4, 8), 'Unsupported Android network map format'
    key = C.c_uint32(60000)
    value = C.create_string_buffer(8)
    attr = struct.pack('I4xQQQ', fd, C.addressof(key), C.addressof(value), 0)
    existed = True
    try:
        bpf(1, attr)
    except OSError as error:
        if error.errno != errno.ENOENT:
            raise
        existed = False
    interface, rules = struct.unpack('II', value.raw)
    background_match = 1 << 12
    if '--remove' in sys.argv:
        if existed and rules & background_match:
            rules &= ~background_match
            if rules:
                value.raw = struct.pack('II', interface, rules)
                bpf(2, attr)
            else:
                bpf(3, attr)
        report('Ubuntu background network allowance removed')
    elif not rules & background_match:
        value.raw = struct.pack('II', interface, rules | background_match)
        bpf(2, attr)
        report('Ubuntu UID 60000 background networking allowed')
    else:
        report('Ubuntu background networking already allowed')
finally:
    os.close(fd)
