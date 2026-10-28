#!/usr/bin/env python3
"""解码 .prn 里的 JBIG 页，输出尺寸与四象限墨量（用于客观验证背面旋转）。

libjbig 是 .deb 的运行时依赖，系统里必有，用 ctypes 直接调，不需要 jbigkit 工具包。

用法：
    python3 jbigdec.py job.prn --list           # 列出各页的 IMAGELEN/尺寸
    python3 jbigdec.py job.prn --page 2 --pbm out.pbm
    python3 jbigdec.py job.prn --page 2 --quad  # 只看四象限墨量
"""
import argparse
import ctypes
import ctypes.util
import sys


def find_lib():
    for name in (ctypes.util.find_library('jbig'), 'libjbig.so.0', 'libjbig.so'):
        if not name:
            continue
        try:
            return ctypes.CDLL(name)
        except OSError:
            continue
    return None


def extract_pages(data):
    """返回 [(imagelen, payload_bytes)]，按出现顺序。"""
    out = []
    pos = 0
    key = b'@PJL SET IMAGELEN='
    while True:
        i = data.find(key, pos)
        if i < 0:
            break
        j = data.find(b'\r\n', i)
        if j < 0:
            break
        try:
            n = int(data[i + len(key):j])
        except ValueError:
            pos = j + 2
            continue
        start = j + 2
        out.append((n, data[start:start + n]))
        pos = start
    return out


class Decoder:
    def __init__(self, lib):
        self.lib = lib
        self.lib.jbg_dec_init.argtypes = [ctypes.c_void_p]
        self.lib.jbg_dec_init.restype = None      # 注意：jbig.h 里是 void，不是 int
        self.lib.jbg_dec_in.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        self.lib.jbg_dec_in.restype = ctypes.c_int
        self.lib.jbg_dec_getwidth.argtypes = [ctypes.c_void_p]
        self.lib.jbg_dec_getwidth.restype = ctypes.c_ulong
        self.lib.jbg_dec_getheight.argtypes = [ctypes.c_void_p]
        self.lib.jbg_dec_getheight.restype = ctypes.c_ulong
        self.lib.jbg_dec_getimage.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.jbg_dec_getimage.restype = ctypes.POINTER(ctypes.c_ubyte)
        self.lib.jbg_dec_getsize.argtypes = [ctypes.c_void_p]
        self.lib.jbg_dec_getsize.restype = ctypes.c_size_t

    def decode(self, payload):
        # jbg_dec_state 结构体由库自己管理，给足空间即可
        state = ctypes.create_string_buffer(1 << 20)
        self.lib.jbg_dec_init(state)
        cnt = ctypes.c_size_t(0)
        rc = self.lib.jbg_dec_in(state, payload, len(payload), ctypes.byref(cnt))
        if rc != 0:
            raise RuntimeError('jbg_dec_in rc=%d (consumed %d/%d)'
                               % (rc, cnt.value, len(payload)))
        w = self.lib.jbg_dec_getwidth(state)
        h = self.lib.jbg_dec_getheight(state)
        ptr = self.lib.jbg_dec_getimage(state, 0)
        n = self.lib.jbg_dec_getsize(state)
        if not ptr or n == 0:
            raise RuntimeError('decode gave empty image')
        return w, h, bytes(bytearray(ptr[:n]))


def quad_ink(buf, w, h):
    bpl = (w + 7) // 8
    q = [0, 0, 0, 0]
    for y in range(h):
        row = buf[y * bpl:(y + 1) * bpl]
        top = y < h // 2
        for x in range(w):
            if (row[x >> 3] >> (7 - (x & 7))) & 1:
                left = x < w // 2
                q[(0 if top else 2) + (0 if left else 1)] += 1
    return q


def write_pbm(path, buf, w, h):
    with open(path, 'wb') as f:
        f.write(b'P4\n%d %d\n' % (w, h))
        f.write(buf[:((w + 7) // 8) * h])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--page', type=int, default=1)
    ap.add_argument('--pbm')
    ap.add_argument('--quad', action='store_true')
    a = ap.parse_args()

    data = open(a.file, 'rb').read()
    pages = extract_pages(data)
    if not pages:
        print('没有找到 @PJL SET IMAGELEN=', file=sys.stderr)
        return 1

    lib = find_lib()
    if not lib:
        print('找不到 libjbig', file=sys.stderr)
        return 1
    dec = Decoder(lib)

    if a.list:
        for i, (n, p) in enumerate(pages, 1):
            try:
                w, h, _ = dec.decode(p)
                print('page %d: imagelen=%d  decoded=%dx%d' % (i, n, w, h))
            except Exception as e:
                print('page %d: imagelen=%d  解码失败: %s' % (i, n, e))
        return 0

    if a.page < 1 or a.page > len(pages):
        print('页号越界（共 %d 页）' % len(pages), file=sys.stderr)
        return 1
    n, payload = pages[a.page - 1]
    w, h, buf = dec.decode(payload)
    print('page %d: %dx%d  imagelen=%d' % (a.page, w, h, n))

    if a.quad:
        q = quad_ink(buf, w, h)
        total = sum(q) or 1
        print('四象限墨量  左上=%d (%.1f%%)  右上=%d (%.1f%%)'
              % (q[0], 100.0 * q[0] / total, q[1], 100.0 * q[1] / total))
        print('            左下=%d (%.1f%%)  右下=%d (%.1f%%)'
              % (q[2], 100.0 * q[2] / total, q[3], 100.0 * q[3] / total))

    if a.pbm:
        write_pbm(a.pbm, buf, w, h)
        print('已写出', a.pbm)
    return 0


if __name__ == '__main__':
    sys.exit(main())
