#!/usr/bin/env python3
"""Windows 端：枚举/设置 GDI 打印队列的 DEVMODE（双面、纸盒），并把渲染结果打到文件端口。

用 ctypes 直接调 winspool.drv，避免依赖 pywin32 或 Add-Type。

用法：
  python wincap.py enum <printer> <port>
  python wincap.py set  <printer> <port> <duplex 1|2|3> <source binid>

duplex: 1=单面 2=长边(VERTICAL) 3=短边(HORIZONTAL)
source: DMBIN 编号，见 enum 输出
"""
import ctypes
import struct
import sys
from ctypes import wintypes

winspool = ctypes.WinDLL('winspool.drv', use_last_error=True)

winspool.OpenPrinterW.argtypes = [wintypes.LPWSTR, ctypes.POINTER(wintypes.HANDLE), ctypes.c_void_p]
winspool.OpenPrinterW.restype = wintypes.BOOL
winspool.ClosePrinter.argtypes = [wintypes.HANDLE]
winspool.ClosePrinter.restype = wintypes.BOOL
winspool.DocumentPropertiesW.argtypes = [wintypes.HWND, wintypes.HANDLE, wintypes.LPWSTR,
                                         ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD]
winspool.DocumentPropertiesW.restype = ctypes.c_long
winspool.SetPrinterW.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
winspool.SetPrinterW.restype = wintypes.BOOL
winspool.DeviceCapabilitiesW.argtypes = [wintypes.LPWSTR, wintypes.LPWSTR, wintypes.WORD,
                                         ctypes.c_void_p, ctypes.c_void_p]
winspool.DeviceCapabilitiesW.restype = ctypes.c_int

DC_BINS = 6
DC_DUPLEX = 7
DC_BINNAMES = 12
DM_OUT_BUFFER = 2
DM_IN_BUFFER = 8
DM_DEFAULTSOURCE = 0x00000200
DM_DUPLEX = 0x00001000
CCHBINNAME = 24


def open_printer(name):
    h = wintypes.HANDLE()
    if not winspool.OpenPrinterW(name, ctypes.byref(h), None):
        raise OSError(ctypes.get_last_error(), f'OpenPrinterW({name}) 失败')
    return h


def get_devmode(h, name):
    size = winspool.DocumentPropertiesW(None, h, name, None, None, 0)
    if size <= 0:
        raise OSError('DocumentProperties 取尺寸失败')
    buf = ctypes.create_string_buffer(size)
    rc = winspool.DocumentPropertiesW(None, h, name, buf, None, DM_OUT_BUFFER)
    if rc < 0:
        raise OSError(f'DocumentProperties(DM_OUT_BUFFER) 失败 rc={rc}')
    return buf


def enum(printer, port):
    can = winspool.DeviceCapabilitiesW(printer, port, DC_DUPLEX, None, None)
    print(f'双面能力 DC_DUPLEX = {can} (1=支持)')
    n = winspool.DeviceCapabilitiesW(printer, port, DC_BINNAMES, None, None)
    print(f'纸盒数量 = {n}')
    if n > 0:
        stride = CCHBINNAME * 2  # 宽字符：每个名字 24 个 WCHAR = 48 字节
        nbuf = ctypes.create_string_buffer(stride * n)
        rc = winspool.DeviceCapabilitiesW(printer, port, DC_BINNAMES, nbuf, None)
        wbuf = (wintypes.WORD * n)()
        rc2 = winspool.DeviceCapabilitiesW(printer, port, DC_BINS,
                                           ctypes.cast(wbuf, ctypes.c_void_p), None)
        for i in range(min(rc, rc2, n)):
            raw = nbuf.raw[i * stride:(i + 1) * stride]
            nm = raw.decode('utf-16-le', 'ignore').split('\x00')[0]
            print(f'  bin={wbuf[i]:3d}  name="{nm}"')


def set_devmode(printer, port, duplex, source):
    h = open_printer(printer)
    try:
        buf = get_devmode(h, printer)
        data = bytearray(buf.raw)
        fields = struct.unpack_from('<I', data, 72)[0]
        fields |= DM_DUPLEX | DM_DEFAULTSOURCE
        struct.pack_into('<I', data, 72, fields)
        struct.pack_into('<H', data, 88, source)   # dmDefaultSource
        struct.pack_into('<H', data, 94, duplex)   # dmDuplex
        newbuf = ctypes.create_string_buffer(bytes(data), len(data))
        rc = winspool.DocumentPropertiesW(None, h, printer, newbuf, newbuf,
                                          DM_IN_BUFFER | DM_OUT_BUFFER)
        if rc < 0:
            raise OSError(f'DocumentProperties(merge) 失败 rc={rc}')
        # 读回最终值
        got_dup = struct.unpack_from('<H', newbuf.raw, 94)[0]
        got_src = struct.unpack_from('<H', newbuf.raw, 88)[0]
        f2 = struct.unpack_from('<I', newbuf.raw, 72)[0]

        class PRINTER_INFO_9(ctypes.Structure):
            _fields_ = [('pDevMode', ctypes.c_void_p)]

        pi9 = PRINTER_INFO_9(ctypes.cast(newbuf, ctypes.c_void_p))
        ok = winspool.SetPrinterW(h, 9, ctypes.byref(pi9), 0)
        print(f'SetPrinter(level9) ok={ok} err={ctypes.get_last_error()}')
        print(f'最终 dmDuplex={got_dup} dmDefaultSource={got_src} dmFields=0x{f2:08x}')
    finally:
        winspool.ClosePrinter(h)


def read_devmode(printer):
    h = open_printer(printer)
    try:
        buf = get_devmode(h, printer)
        dup = struct.unpack_from('<H', buf.raw, 94)[0]
        src = struct.unpack_from('<H', buf.raw, 88)[0]
        fld = struct.unpack_from('<I', buf.raw, 72)[0]
        dmsize = struct.unpack_from('<H', buf.raw, 68)[0]
        dmextra = struct.unpack_from('<H', buf.raw, 70)[0]
        print(f'dmDuplex={dup} dmDefaultSource={src} dmFields=0x{fld:08x} '
              f'dmSize={dmsize} dmDriverExtra={dmextra} total={len(buf.raw)}')
    finally:
        winspool.ClosePrinter(h)


def dump_devmode(printer, outfile):
    h = open_printer(printer)
    try:
        buf = get_devmode(h, printer)
        with open(outfile, 'wb') as f:
            f.write(buf.raw)
        dmsize = struct.unpack_from('<H', buf.raw, 68)[0]
        dmextra = struct.unpack_from('<H', buf.raw, 70)[0]
        print(f'wrote {outfile} total={len(buf.raw)} dmSize={dmsize} dmDriverExtra={dmextra}')
    finally:
        winspool.ClosePrinter(h)


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'enum':
        enum(sys.argv[2], sys.argv[3])
    elif cmd == 'set':
        set_devmode(sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]))
    elif cmd == 'read':
        read_devmode(sys.argv[2])
    elif cmd == 'dump':
        dump_devmode(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
