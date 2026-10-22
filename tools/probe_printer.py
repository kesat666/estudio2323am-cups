#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
e-STUDIO 2323AM 协议探测工具

目的：在动手逆向私有 GDI 协议之前，先确认这台机器是否其实支持标准协议
(IPP / PCL5e / PostScript / PJL)。一旦命中，ARM64 支持可以降级为"装通用驱动"。

默认只做不发作业的探测，不会出纸。
加 --test-pcl5 / --test-ps 才会真的打印测试页（会消耗纸张）。

用法:
    python probe_printer.py 192.168.1.50
    python probe_printer.py 192.168.1.50 --test-pcl5 --test-ps
    python probe_printer.py 192.168.1.50 --ipp-path /ipp/print --timeout 4

仅依赖 Python 标准库。
"""

import argparse
import re
import socket
import struct
import sys

# ---------------------------------------------------------------- 基础工具


def tcp_probe(host, port, timeout=3.0):
    """TCP 端口是否可连接。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect((host, port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def send_raw(host, port, payload, timeout=5.0, read_back=True):
    """向 RAW 端口发数据，可选回读反向通道。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    reply = b""
    try:
        s.connect((host, port))
    except OSError:
        s.close()
        return None
    try:
        s.sendall(payload)
        if read_back:
            s.settimeout(2.0)
            chunks = []
            while True:
                try:
                    b = s.recv(4096)
                except socket.timeout:
                    break
                if not b:
                    break
                chunks.append(b)
            reply = b"".join(chunks)
    finally:
        s.close()
    return reply


def _attr(tag, name, value):
    """IPP 属性编码: tag + name-len + name + value-len + value"""
    n = name.encode("ascii")
    v = value.encode("utf-8")
    return bytes([tag]) + struct.pack(">H", len(n)) + n + struct.pack(">H", len(v)) + v


def ipp_get_printer_attributes(host, path="/ipp/print"):
    """构造最小 IPP Get-Printer-Attributes 请求，返回 HTTP 状态与响应体。"""
    body = b"\x02\x00"                      # version 2.0
    body += b"\x00\x0b"                     # operation-id: Get-Printer-Attributes
    body += struct.pack(">I", 1)            # request-id
    body += b"\x01"                         # operation-attributes-tag
    body += _attr(0x47, "attributes-charset", "utf-8")     # charset
    body += _attr(0x48, "natural-language", "en")          # naturalLanguage
    body += _attr(0x45, "printer-uri", "ipp://%s%s" % (host, path))  # uri
    body += b"\x03"                         # end-of-attributes

    req = (
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/ipp\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n" % (path, host, len(body))
    ).encode("ascii") + body

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(6.0)
    raw = b""
    try:
        s.connect((host, 631))
        s.sendall(req)
        while True:
            b = s.recv(8192)
            if not b:
                break
            raw += b
            if len(raw) > 512 * 1024:
                break
    except OSError:
        return None, b""
    finally:
        s.close()

    m = re.match(rb"HTTP/\d\.\d\s+(\d{3})", raw)
    status = int(m.group(1)) if m else 0
    head, _, payload = raw.partition(b"\r\n\r\n")
    return status, payload


def printable_runs(data, minlen=4):
    """从二进制里抽出可读字符串，用于粗看 IPP 响应。"""
    out = []
    cur = []
    for byte in data:
        if 32 <= byte < 127:
            cur.append(chr(byte))
        else:
            if len(cur) >= minlen:
                out.append("".join(cur))
            cur = []
    if len(cur) >= minlen:
        out.append("".join(cur))
    return out


def http_title(host, port=80, timeout=4.0):
    """抓 TopAccess 首页标题，用于确认机型。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    raw = b""
    try:
        s.connect((host, port))
        s.sendall(("GET / HTTP/1.0\r\nHost: %s\r\n\r\n" % host).encode("ascii"))
        while True:
            b = s.recv(8192)
            if not b:
                break
            raw += b
            if len(raw) > 256 * 1024:
                break
    except OSError:
        return None
    finally:
        s.close()
    text = raw.decode("latin-1", "ignore")
    m = re.search(r"<title[^>]*>(.*?)</title>", text, re.I | re.S)
    return m.group(1).strip() if m else ""


# ---------------------------------------------------------------- 报文构造

UEL = b"\x1b%-12345X"


def pjl_info_id():
    return UEL + b"@PJL INFO ID\r\n" + UEL + b"\r\n"


def pcl5_page(text="PCL5 PROBE OK"):
    """最小 PCL5 页面：复位、横向、文本、换页。"""
    return (
        UEL
        + b"@PJL COMMENT probe\r\n"
        + b"@PJL ENTER LANGUAGE=PCL\r\n"
        + b"\x1bE"                    # 复位
        + b"\x1b&l0O"                 # 纵向
        + b"\x1b&l0E"                 # 上边距 0
        + b"\x1b(s0p12h10v0s0b3T"     # Courier 12pt
        + b"\x1b&a100H"               # 水平位置
        + b"\x1b&a200V"               # 垂直位置
        + text.encode("ascii")
        + b"\x0c"                     # 换页
        + UEL
    )


def ps_page(text="PS PROBE OK"):
    """最小 PostScript 页面。"""
    ps = (
        "%%!PS-Adobe-3.0\n"
        "%%%%BoundingBox: 0 0 595 842\n"
        "/Courier findfont 24 scalefont setfont\n"
        "72 700 moveto (%s) show\n"
        "showpage\n"
        "%%%%EOF\n" % text
    )
    return UEL + b"@PJL ENTER LANGUAGE=POSTSCRIPT\r\n" + ps.encode("ascii") + UEL


# ---------------------------------------------------------------- 主流程


def main():
    ap = argparse.ArgumentParser(description="e-STUDIO 2323AM 协议探测")
    ap.add_argument("host", help="打印机 IP 或主机名")
    ap.add_argument("--port", type=int, default=9100, help="RAW 端口，默认 9100")
    ap.add_argument("--timeout", type=float, default=4.0, help="超时秒数")
    ap.add_argument("--ipp-path", default="/ipp/print", help="IPP 资源路径")
    ap.add_argument("--test-pcl5", action="store_true", help="打印一页 PCL5 测试页（出纸）")
    ap.add_argument("--test-ps", action="store_true", help="打印一页 PostScript 测试页（出纸）")
    args = ap.parse_args()

    host = args.host
    print("目标设备: %s" % host)
    print("=" * 60)

    # --- 1. 端口扫描
    print("\n[1] 端口探测")
    ports = [(args.port, "RAW/打印作业"), (631, "IPP"), (80, "TopAccess/HTTP"),
             (443, "HTTPS"), (515, "LPD"), (161, "SNMP(仅检测可达)")]
    open_ports = []
    for port, desc in ports:
        ok = tcp_probe(host, port, args.timeout)
        print("    %-5d %-16s %s" % (port, desc, "OPEN" if ok else "-"))
        if ok:
            open_ports.append(port)

    # --- 2. TopAccess 页面
    if 80 in open_ports:
        print("\n[2] TopAccess 首页")
        title = http_title(host)
        print("    title: %s" % (title if title else "(空)"))

    # --- 3. PJL 回读
    print("\n[3] PJL 通道回读 (INFO ID)")
    reply = send_raw(host, args.port, pjl_info_id(), args.timeout)
    if reply is None:
        print("    RAW 端口连不上，跳过（检查 IP / 端口 / 网络）")
    elif reply:
        print("    收到回读 %d 字节:" % len(reply))
        for line in printable_runs(reply, 3):
            print("      %s" % line)
        print("    => 有 PJL 通道，存在标准作业层，逆向难度显著下降")
    else:
        print("    无回读。GDI 机型常见结果：RAW 数据被直接交给私有解释器。")

    # --- 4. IPP
    print("\n[4] IPP Get-Printer-Attributes")
    if 631 not in open_ports:
        print("    631 未开放，跳过")
    else:
        status, payload = ipp_get_printer_attributes(host, args.ipp_path)
        if status is None:
            print("    连接失败")
        elif status == 0:
            print("    无 HTTP 响应")
        else:
            print("    HTTP %d, 响应体 %d 字节" % (status, len(payload)))
            runs = printable_runs(payload, 4)
            keys = [r for r in runs if "-" in r and len(r) < 40]
            print("    属性名样本: %s" % (", ".join(keys[:20]) if keys else "(无)"))
            hit = [r for r in runs if re.search(r"(?i)model|make|printer-name", r)]
            if hit:
                print("    命中: %s" % ", ".join(hit[:6]))
                print("    => **支持 IPP，ARM64 可直接免驱 (Microsoft IPP Class Driver)**")
        print("    提示：也试一下 --ipp-path / 或 /printers 等路径。")

    # --- 5. 可选：实打测试页
    if args.test_pcl5:
        print("\n[5] PCL5 测试页（会出纸）")
        send_raw(host, args.port, pcl5_page("PCL5 PROBE OK"), args.timeout, read_back=False)
        print("    已发送。若正常出纸且文字清晰 => 存在 PCL5e 解释器，可用通用驱动")

    if args.test_ps:
        print("\n[6] PostScript 测试页（会出纸）")
        send_raw(host, args.port, ps_page("PS PROBE OK"), args.timeout, read_back=False)
        print("    已发送。若正常出纸 => 存在 PS3 解释器")

    if not (args.test_pcl5 or args.test_ps):
        print("\n(未做打印测试。加 --test-pcl5 / --test-ps 才会出纸。)")

    print("\n" + "=" * 60)
    print("判定口诀：IPP 通 => 免驱；PCL/PS 出纸 => 通用驱动；全哑 => 私有 GDI 协议，需逆向。")


if __name__ == "__main__":
    sys.exit(main())
