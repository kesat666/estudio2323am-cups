#!/usr/bin/env python3
"""e-STUDIO 2323AM 作业流解析器 / 重放器。

从 Windows 驱动输出的 .prn 里解析出完整的作业结构，再用解析结果重新组包，
与原始文件逐字节比对。一致 = 对协议的理解无误，可以直接照着写 filter。

结构（已由实测确认，见 docs/protocol-re.md）
------------------------------------------
    UEL  "\\x1b%-12345X"
    "@PJL\\r\\n"
    "@PJL SET <KEY>=<VALUE>\\r\\n"   x N     ASCII，顺序固定
    "@PJL SET IMAGELEN=<n>\\r\\n"
    <JBIG BIE>                       n 字节，T.82 标准，非私有格式
    "@PJL SET DOTCOUNT=<n>\\r\\n"
    "@PJL SET PAGESTATUS=END\\r\\n"
    "@PJL EOJ\\r\\n"
    UEL  "\\x1b%-12345X\\r\\n"

用法
----
    python tools/pjl_probe.py --parse C:\\captures\\white.prn
    python tools/pjl_probe.py --selftest C:\\captures\\white.prn
"""

import argparse
import struct
import sys

UEL = b"\x1b%-12345X"
CRLF = b"\r\n"


class Job:
    """一页作业：PJL 字段 + 数据段 + 尾部字段。"""

    def __init__(self):
        self.sets = []          # [(key, value)] 顺序保留
        self.image = b""
        self.image_len = 0
        self.trailer = []       # 尾部 @PJL SET（DOTCOUNT / PAGESTATUS）
        self.eoj = False
        self.tail_uel = False
        self.raw_head = b""

    # ---- 解析 ----

    @classmethod
    def parse(cls, data):
        j = cls()
        p = 0
        seen_image = False
        if data.startswith(UEL):
            p = len(UEL)
        if data[p:p + 2] == CRLF:
            p += 2
        if data[p:p + 4] != b"@PJL":
            raise ValueError("偏移 %d 处不是 @PJL（开头可能不是 UEL+PJL 结构）" % p)
        p += 4
        p += 2 if data[p:p + 2] == CRLF else 0

        while True:
            if data[p:p + 4] != b"@PJL":
                break
            e = data.find(CRLF, p)
            if e < 0:
                break
            line = data[p:e]
            p = e + 2
            if line == b"@PJL EOJ":
                j.eoj = True
                break
            if line.startswith(b"@PJL SET "):
                kv = line[9:].decode("ascii", "replace")
                k, _, v = kv.partition("=")
                k, v = k.strip(), v.strip()
                if k == "IMAGELEN":
                    j.image_len = int(v)
                    j.raw_head = data[:p]
                    j.image = data[p:p + j.image_len]
                    p += j.image_len
                    seen_image = True
                elif seen_image:
                    # 数据段之后的 SET 属于尾部（PAGESTATUS=END / DOTCOUNT）
                    j.trailer.append((k, v))
                else:
                    # 数据段之前的 SET 属于头部（含 PAGESTATUS=START），顺序必须保留
                    j.sets.append((k, v))
            # 其它 @PJL 指令忽略

        if data[p:p + len(UEL)] == UEL:
            j.tail_uel = True
        return j

    # ---- 组包 ----

    def build(self):
        out = bytearray()
        out += UEL + b"@PJL" + CRLF
        for k, v in self.sets:
            out += ("@PJL SET %s=%s" % (k, v)).encode("ascii") + CRLF
        out += ("@PJL SET IMAGELEN=%d" % len(self.image)).encode("ascii") + CRLF
        out += self.image
        for k, v in self.trailer:
            out += ("@PJL SET %s=%s" % (k, v)).encode("ascii") + CRLF
        if self.eoj:
            out += b"@PJL EOJ" + CRLF
        if self.tail_uel:
            out += UEL + CRLF
        return bytes(out)


def parse_bie(seg):
    """解析 JBIG BIE 头（T.82）。返回字段字典。"""
    if len(seg) < 21:
        return None
    dl, d, rp, l0, mx = struct.unpack(">IIIII", seg[:20])
    my = seg[20]
    return {"DL": dl, "D": d, "RP": rp, "L0": l0, "MX": mx, "MY": my,
            "rest": seg[21:29]}


def cmd_parse(path):
    data = open(path, "rb").read()
    j = Job.parse(data)
    print("文件        : %s" % path)
    print("总长        : %d 字节" % len(data))
    print("PJL 头长度  : %d 字节" % len(j.raw_head))
    print()
    print("--- PJL 字段（按原顺序）---")
    for k, v in j.sets:
        print("  %-14s = %s" % (k, v))
    print("  %-14s = %d" % ("IMAGELEN", j.image_len))
    print()
    print("--- 数据段 ---")
    b = parse_bie(j.image)
    if b:
        w = int(next((v for k, v in j.sets if k == "PAPERWIDTH"), 0))
        h = int(next((v for k, v in j.sets if k == "PAPERLENGTH"), 0))
        print("  BIE 头    : DL=%d  D=%d  RP=%d  L0=%d  MX=%d  MY=%d"
              % (b["DL"], b["D"], b["RP"], b["L0"], b["MX"], b["MY"]))
        print("  头后 8 字节: %s" % " ".join("%02x" % x for x in b["rest"]))
        print("  与 PAPERWIDTH/LENGTH 对照: D %s %d, RP %s %d"
              % ("==" if b["D"] == w else "!=", w, "==" if b["RP"] == h else "!=", h))
    print("  数据长度  : %d 字节" % len(j.image))
    raw = w * h // 8 if b else 0
    if raw:
        print("  1bpp 原始 : %d 字节 -> 压缩比 %.4f" % (raw, len(j.image) / raw))
    print()
    print("--- 尾部 ---")
    for k, v in j.trailer:
        print("  %-14s = %s" % (k, v))
    print("  @PJL EOJ    : %s" % ("有" if j.eoj else "无"))
    print("  结尾 UEL    : %s" % ("有" if j.tail_uel else "无"))
    return 0


def cmd_selftest(path):
    data = open(path, "rb").read()
    j = Job.parse(data)
    rebuilt = j.build()

    print("原文      : %d 字节" % len(data))
    print("重放      : %d 字节" % len(rebuilt))
    if rebuilt == data:
        print("结果      : 逐字节一致 -> 对 PJL 层的理解完全正确")
        return 0

    print("结果      : 不一致，定位第一处差异")
    n = min(len(rebuilt), len(data))
    for i in range(n):
        if rebuilt[i] != data[i]:
            print("  首个差异 @ 0x%04x: 原文 %02x / 重放 %02x"
                  % (i, data[i], rebuilt[i]))
            print("  原文: %r" % data[max(0, i - 24):i + 24])
            print("  重放: %r" % rebuilt[max(0, i - 24):i + 24])
            break
    else:
        print("  前缀相同，长度不同")
    return 1


def main():
    ap = argparse.ArgumentParser(description="e-STUDIO 2323AM 作业流解析 / 重放")
    ap.add_argument("file", help=".prn 或 .bin（驱动输出的作业流）")
    ap.add_argument("--parse", action="store_true", help="解析并打印结构")
    ap.add_argument("--selftest", action="store_true", help="重新组包并逐字节比对")
    a = ap.parse_args()
    if a.selftest:
        return cmd_selftest(a.file)
    return cmd_parse(a.file)


if __name__ == "__main__":
    sys.exit(main())
