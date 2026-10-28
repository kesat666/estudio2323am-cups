#!/usr/bin/env python3
"""通过 libusb 直接向打印机发 PJL 查询并读回响应（只读，不改设备状态）。

用途：确认打印机到底支持哪些 PJL 变量、变量当前值是什么。
CUPS 的 usb:// 后端也是走这条通道，所以响应就是设备真实认知。

依赖：libusb-1.0.so.0（Kylin/UOS 自带），无需 pyusb。
用法：sudo python3 pjlq.py [cmd ...]    不带参数则跑默认查询集
"""
import ctypes
import ctypes.util
import sys
import time

VID, PID = 0x08A6, 0x8056
IFACE = 1
EP_OUT, EP_IN = 0x01, 0x82
UEL = b'\x1b%-12345X'


class Timeval(ctypes.Structure):
    _fields_ = [('tv_sec', ctypes.c_long), ('tv_usec', ctypes.c_long)]


def load():
    name = ctypes.util.find_library('usb-1.0') or 'libusb-1.0.so.0'
    return ctypes.CDLL(name)


def main(argv):
    lib = load()
    lib.libusb_init.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    lib.libusb_init.restype = ctypes.c_int
    lib.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
    lib.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
    lib.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.libusb_claim_interface.restype = ctypes.c_int
    lib.libusb_release_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.libusb_kernel_driver_active.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.libusb_kernel_driver_active.restype = ctypes.c_int
    lib.libusb_detach_kernel_driver.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.libusb_detach_kernel_driver.restype = ctypes.c_int
    lib.libusb_bulk_transfer.argtypes = [
        ctypes.c_void_p, ctypes.c_ubyte, ctypes.POINTER(ctypes.c_ubyte),
        ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.c_uint]
    lib.libusb_bulk_transfer.restype = ctypes.c_int
    lib.libusb_close.argtypes = [ctypes.c_void_p]
    lib.libusb_exit.argtypes = [ctypes.c_void_p]

    ctx = ctypes.c_void_p()
    if lib.libusb_init(ctypes.byref(ctx)) != 0:
        print('libusb_init 失败', file=sys.stderr)
        return 1
    dev = lib.libusb_open_device_with_vid_pid(ctx, VID, PID)
    if not dev:
        print('找不到 %04x:%04x 设备（是否被占用？）' % (VID, PID), file=sys.stderr)
        lib.libusb_exit(ctx)
        return 1

    if lib.libusb_kernel_driver_active(dev, IFACE) == 1:
        lib.libusb_detach_kernel_driver(dev, IFACE)
    rc = lib.libusb_claim_interface(dev, IFACE)
    if rc != 0:
        print('claim_interface 失败 rc=%d' % rc, file=sys.stderr)
        lib.libusb_close(dev)
        lib.libusb_exit(ctx)
        return 1

    def bulk_out(data, timeout=5000):
        buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        sent = ctypes.c_int(0)
        rc = lib.libusb_bulk_transfer(dev, EP_OUT, buf, len(data), ctypes.byref(sent), timeout)
        return rc, sent.value

    def bulk_in(maxlen=8192, timeout=3000):
        buf = (ctypes.c_ubyte * maxlen)()
        got = ctypes.c_int(0)
        rc = lib.libusb_bulk_transfer(dev, EP_IN, buf, maxlen, ctypes.byref(got), timeout)
        return rc, bytes(buf[:got.value])

    cmds = argv[1:] or [
        '@PJL INFO ID',
        '@PJL INFO STATUS',
        '@PJL DINQUIRE DUPLEX',
        '@PJL DINQUIRE BINDING',
        '@PJL DINQUIRE MEDIASOURCE',
        '@PJL DINQUIRE PAPER',
        '@PJL DINQUIRE MEDIATYPE',
        '@PJL INFO VARIABLES',
    ]

    for c in cmds:
        payload = UEL + c.encode() + b'\r\n' + UEL
        rc, n = bulk_out(payload)
        print('=' * 70)
        print('>>>', c, ' (out rc=%d n=%d)' % (rc, n))
        # 先清掉可能残留的数据
        deadline = time.time() + 3.0
        chunks = []
        while time.time() < deadline:
            rc, data = bulk_in()
            if data:
                chunks.append(data)
                deadline = time.time() + 0.8  # 有数据就再等一会儿
            else:
                break
        raw = b''.join(chunks)
        print('<<< %d bytes' % len(raw))
        if raw:
            try:
                print(raw.decode('utf-8', 'replace'))
            except Exception:
                print(repr(raw[:800]))
        else:
            print('(无响应)')

    lib.libusb_release_interface(dev, IFACE)
    lib.libusb_close(dev)
    lib.libusb_exit(ctx)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
