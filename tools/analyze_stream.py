#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
analyze_stream.py — 分析从打印机抓到的原始 payload

配合 docs/protocol-re.md 使用。输入是 Wireshark "Follow TCP Stream → Raw"
导出的纯 payload，或你从 pcap 里抽出来的单方向数据。

用法:
    # 总体特征：长度、熵、重复字节、游程、疑似压缩方式
    python analyze_stream.py --stats white.bin

    # 找位图起点：按窗口看熵的变化
    python analyze_stream.py --entropy white.bin --window 512

    # 差分两个抓包（核心手法）
    python analyze_stream.py --diff white.bin black.bin

    # 和理论原始位图大小对比，判断有没有压缩
    python analyze_stream.py --stats white.bin --page a4-600

    # 看某一段的十六进制
    python analyze_stream.py --hexdump white.bin --offset 0 --length 256

纯标准库，无依赖。
"""

import argparse
import math
import sys

# A4 / A3 / Letter 在 600dpi 下 1bpp 的原始字节数（未扣边距）
PAGE_RAW = {
    "a4-600": (4960, 7016),
    "a3-600": (7016, 9921),
    "letter-600": (5100, 6600),
}


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def entropy(data):
    if not data:
        return 0.0
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    n = len(data)
    e = 0.0
    for c in counts:
        if c:
            p = c / n
            e -= p * math.log2(p)
    return e


def runs(data, limit=8):
    """返回最长的几个同字节游程 (长度, 字节, 偏移)。"""
    out = []
    if not data:
        return out
    cur = data[0]
    start = 0
    for i in range(1, len(data)):
        if data[i] != cur:
            if i - start >= limit:
                out.append((i - start, cur, start))
            cur = data[i]
            start = i
    if len(data) - start >= limit:
        out.append((len(data) - start, cur, start))
    out.sort(reverse=True)
    return out[:10]


def markers(data):
    """识别常见结构标记。"""
    hits = []
    if data.startswith(b"\x1b%-12345X"):
        hits.append("UEL (@PJL 通用退出语言) 开头")
    if b"@PJL" in data[:4096]:
        hits.append("含 @PJL 指令（说明有 PJL 作业层）")
    if b"\xff\xd8\xff" in data:
        hits.append("含 JPEG SOI (FF D8 FF) —— 疑似 JPEG 压缩")
    if b"\x1bE" in data[:4096]:
        hits.append("含 ESC E —— 疑似 PCL 复位")
    if b"%!PS" in data[:4096]:
        hits.append("含 %!PS —— 疑似 PostScript")
    return hits


def cmd_stats(path, page=None):
    data = read_file(path)
    n = len(data)
    print("文件      : %s" % path)
    print("长度      : %d 字节 (%.2f KiB / %.2f MiB)" % (n, n / 1024, n / 1048576))
    print("熵        : %.3f bit/byte" % entropy(data))

    counts = [0] * 256
    for b in data:
        counts[b] += 1
    top = sorted(range(256), key=lambda i: -counts[i])[:8]
    print("高频字节  : %s" % ", ".join("0x%02X(%d)" % (b, counts[b]) for b in top if counts[b]))
    print("首 32 字节: %s" % data[:32].hex(" "))

    mk = markers(data)
    if mk:
        print("结构标记  :")
        for m in mk:
            print("            - %s" % m)

    rs = runs(data)
    if rs:
        print("最长游程  :")
        for ln, b, off in rs:
            print("            %8d × 0x%02X  @0x%08X" % (ln, b, off))

    # 零字节占比：位图区常出现大量 0x00
    if n:
        zeros = counts[0] / n
        print("0x00 占比 : %.1f%%" % (zeros * 100))

    if page:
        w, h = PAGE_RAW[page]
        raw = w * h // 8
        print("\n理论原始大小 (%s, 1bpp): %d 字节 (%.2f MiB)" % (page, raw, raw / 1048576))
        if n > 0:
            ratio = n / raw
            print("实测/理论 : %.3f" % ratio)
            if ratio > 0.9:
                print("推断      : 基本无压缩（直传位图）")
            elif ratio > 0.2:
                print("推断      : 有中等压缩（RLE/行级编码）")
            else:
                print("推断      : 强压缩（G3/G4/JBIG/JPEG 或大量空白页）")
    return 0


def cmd_entropy(path, window):
    data = read_file(path)
    n = len(data)
    if n == 0:
        print("空文件")
        return 1
    print("窗口熵 (%d 字节/格)，用于定位位图数据起点" % window)
    print("偏移        熵    直方图")
    for off in range(0, n, window):
        chunk = data[off:off + window]
        e = entropy(chunk)
        bar = "#" * int(round(e / 8 * 40))
        print("0x%08X  %5.2f  %s" % (off, e, bar))
    return 0


def _spans(a, b, gap=32):
    """把差异位置合并成区间（间隔小于 gap 的合并）。"""
    n = min(len(a), len(b))
    diffs = [i for i in range(n) if a[i] != b[i]]
    if len(a) != len(b):
        diffs.extend(range(n, max(len(a), len(b))))
    if not diffs:
        return []
    spans = []
    s = p = diffs[0]
    for i in diffs[1:]:
        if i - p <= gap:
            p = i
        else:
            spans.append((s, p))
            s = p = i
    spans.append((s, p))
    return spans


def cmd_diff(pa, pb, gap):
    a = read_file(pa)
    b = read_file(pb)
    print("A: %s (%d 字节)" % (pa, len(a)))
    print("B: %s (%d 字节)" % (pb, len(b)))

    n = min(len(a), len(b))
    same = 0
    while same < n and a[same] == b[same]:
        same += 1
    print("公共前缀  : %d 字节 (0x%X)" % (same, same))
    if same:
        print("           前缀尾部: %s" % a[max(0, same - 16):same].hex(" "))

    spans = _spans(a, b, gap)
    print("差异区间  : %d 处" % len(spans))
    for s, e in spans[:30]:
        ln = e - s + 1
        print("  @0x%08X  len=%-6d  A=%s  B=%s" %
              (s, ln,
               a[s:min(e + 1, s + 8)].hex(" "),
               b[s:min(e + 1, s + 8)].hex(" ")))
    if len(spans) > 30:
        print("  ... 其余 %d 处省略" % (len(spans) - 30))

    if not spans:
        print("两份报文完全相同。")
    return 0


def cmd_hexdump(path, offset, length):
    data = read_file(path)
    chunk = data[offset:offset + length]
    if not chunk:
        print("该范围为空（文件 %d 字节）" % len(data))
        return 1
    base = offset
    for i in range(0, len(chunk), 16):
        row = chunk[i:i + 16]
        hexs = " ".join("%02x" % x for x in row)
        text = "".join(chr(x) if 32 <= x < 127 else "." for x in row)
        print("%08x  %-47s  %s" % (base + i, hexs, text))
    return 0


def main():
    ap = argparse.ArgumentParser(description="打印机 payload 分析")
    ap.add_argument("file", nargs="?", help="payload 文件")
    ap.add_argument("--stats", action="store_true", help="总体特征统计")
    ap.add_argument("--entropy", action="store_true", help="按窗口输出熵")
    ap.add_argument("--window", type=int, default=512, help="熵窗口大小")
    ap.add_argument("--diff", nargs=2, metavar=("A", "B"), help="差分两个抓包")
    ap.add_argument("--gap", type=int, default=32, help="差异区间合并阈值")
    ap.add_argument("--hexdump", action="store_true", help="十六进制转储")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--length", type=lambda s: int(s, 0), default=256)
    ap.add_argument("--page", choices=sorted(PAGE_RAW),
                    help="与理论原始位图大小对比")
    args = ap.parse_args()

    if args.diff:
        return cmd_diff(args.diff[0], args.diff[1], args.gap)

    if not args.file:
        ap.print_help()
        return 2

    if args.hexdump:
        return cmd_hexdump(args.file, args.offset, args.length)
    if args.entropy:
        return cmd_entropy(args.file, args.window)
    return cmd_stats(args.file, args.page)


if __name__ == "__main__":
    sys.exit(main())
