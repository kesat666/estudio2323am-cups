#!/usr/bin/env python3
"""Port 9100 代理：截获 Windows 驱动发给 2323AM 的原始字节流。

为什么需要它
------------
逆向私有协议的唯一输入，是"驱动实际吐出的那串字节"。Wireshark 能抓，但
要处理 TCP 分段、重传、长度截断，还得在一堆包里找作业边界；这个代理更直接：
它是作业流唯一的必经之路，来多少收多少，原样落盘，顺便把回读也存下来。

Windows 驱动只需把端口指向本机（例如 127.0.0.1:19100），代理再转发到真实
打印机 192.168.1.50:9100。打印机照常出纸，文件照常落盘，一次抓一份。

用法
----
    # 起代理（保持前台运行，打印几页就得到几个文件）
    python tools/port9100_proxy.py --target 192.168.1.50

    # 抓一份就叫一个名字，抓完自动退出
    python tools/port9100_proxy.py --target 192.168.1.50 --label white --once

    # 换监听端口 / 输出目录 / 空闲判定
    python tools/port9100_proxy.py --target 192.168.1.50 \
        --listen 19100 --out captures --idle 3.0

产物
----
    captures/white.bin        客户端 -> 打印机（这就是要逆向的作业流）
    captures/white.back.bin   打印机 -> 客户端（PJL 状态回读，字段线索）
    captures/white.meta.json  时间、来源、字节数、SHA256、头部 hexdump

若 white.bin 是 0 字节而 white.back.bin 有数据，说明驱动其实在用双向查询
（SNMP/PJL 取状态）而数据流走了别的端口，此时该改用 Wireshark 全接口抓包。

注意
----
本工具只做转发与记录，不解释、不修改作业内容。打印机行为和直连完全一致。
"""

import argparse
import hashlib
import json
import os
import socket
import sys
import threading
import time

UEL = b"\x1b%-12345X"
DEFAULT_PORT = 9100
CHUNK = 65536


def hexdump(data, limit=512):
    """返回 (hexdump 文本, 可打印预览)，只取前 limit 字节。"""
    head = data[:limit]
    lines = []
    for off in range(0, len(head), 16):
        chunk = head[off:off + 16]
        hexpart = " ".join("%02x" % b for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append("%08x  %-47s  |%s|" % (off, hexpart, text))
    return "\n".join(lines)


def head_summary(data):
    """给出一句人类可读的头部判断，抓包当场就能确认方向。"""
    if not data:
        return "空流（0 字节）"
    if data.startswith(UEL):
        j = data.find(b"@PJL")
        p = data.find(b"ENTER LANGUAGE=")
        lang = ""
        if p >= 0:
            q = data.find(b"\r", p)
            lang = data[p + 15:q if q > 0 else p + 40].decode("ascii", "replace").strip()
        return "UEL + PJL%s" % (" / 语言=" + lang if lang else (" / 有 @PJL" if j >= 0 else ""))
    if data.startswith(b"\x1b@") or data.startswith(b"\x1bE"):
        return "ESC/P 或 PCL 转义序列（\\x1b@ / \\x1bE）"
    if data[:1] == b"\x1b":
        return "以 ESC 开头的未知转义序列"
    if data.startswith(b"%!PS"):
        return "PostScript"
    return "非 PJL 的裸字节流（很可能是私有格式）"


def write_all(path, buf):
    with open(path, "wb") as f:
        f.write(buf)
    return len(buf)


class Session:
    """一次 TCP 连接 = 一个作业。收满即落盘。"""

    def __init__(self, args, index):
        self.args = args
        self.index = index
        self.to_printer = bytearray()
        self.from_printer = bytearray()
        self.lock = threading.Lock()

    def label(self):
        if self.args.label:
            return self.args.label if self.index == 1 else "%s-%d" % (self.args.label, self.index)
        return "capture-%03d" % self.index

    def pump(self, src, sink, store):
        """从 src 读到 store，同时原样转发给 sink；对端关闭即结束。"""
        try:
            while True:
                data = src.recv(CHUNK)
                if not data:
                    break
                with self.lock:
                    store.extend(data)
                try:
                    sink.sendall(data)
                except OSError:
                    break
        except (socket.timeout, OSError):
            pass
        finally:
            try:
                sink.shutdown(socket.SHUT_WR)
            except OSError:
                pass

    def run(self, client, addr):
        args = self.args
        up = None
        try:
            up = socket.create_connection((args.target, args.port),
                                          timeout=args.connect_timeout)
        except OSError as e:
            sys.stderr.write("[FAIL] 连不上打印机 %s:%d -> %s\n"
                             % (args.target, args.port, e))
            sys.stderr.write("       检查线缆/IP/是否只开了 LPR(515) 没开 9100。\n")
            client.close()
            self.dump_only_client(client)
            return

        client.settimeout(args.idle)
        up.settimeout(args.idle)

        t = threading.Thread(target=self.pump,
                             args=(up, client, self.from_printer), daemon=True)
        t.start()
        self.pump(client, up, self.to_printer)
        t.join(timeout=args.idle + 1)
        for s in (client, up):
            try:
                s.close()
            except OSError:
                pass
        self.save(str(addr[0]) if addr else "")

    def dump_only_client(self, client):
        """打印机不可达时，仍然把客户端发来的数据存下来——数据本身仍有价值。"""
        client.settimeout(self.args.idle)
        try:
            while True:
                data = client.recv(CHUNK)
                if not data:
                    break
                self.to_printer.extend(data)
        except (socket.timeout, OSError):
            pass
        client.close()
        self.save("(printer unreachable)")

    def save(self, peer):
        out = self.args.out
        os.makedirs(out, exist_ok=True)
        name = self.label()
        fwd = os.path.join(out, name + ".bin")
        back = os.path.join(out, name + ".back.bin")

        n_fwd = write_all(fwd, bytes(self.to_printer))
        n_back = write_all(back, bytes(self.from_printer))

        meta = {
            "label": name,
            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
            "peer": peer,
            "target": "%s:%d" % (self.args.target, self.args.port),
            "bytes_to_printer": n_fwd,
            "sha256": hashlib.sha256(bytes(self.to_printer)).hexdigest() if n_fwd else "",
            "bytes_from_printer": n_back,
            "head_guess": head_summary(bytes(self.to_printer)),
            "head_hexdump": hexdump(bytes(self.to_printer)),
        }
        with open(os.path.join(out, name + ".meta.json"), "w", encoding="utf-8") as f:
            json.dump(meta, f, ensure_ascii=False, indent=2)

        print("\n[ OK ] %s" % name)
        print("        驱动 -> 打印机 : %d 字节  -> %s" % (n_fwd, fwd))
        print("        打印机 -> 驱动 : %d 字节  -> %s" % (n_back, back))
        print("        头部判断      : %s" % meta["head_guess"])
        if n_fwd:
            print("        前 64 字节    : %s"
                  % " ".join("%02x" % b for b in bytes(self.to_printer)[:64]))
        if n_fwd == 0:
            print("        [WARN] 没有收到数据。可能端口填错，或驱动走了 USB/LPR。")
        print("        下一步: python tools/analyze_stream.py --stats %s --page a4-600" % fwd)


def main():
    ap = argparse.ArgumentParser(
        description="Port 9100 代理：截获驱动发给打印机的原始作业流",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="抓完之后用 tools/analyze_stream.py --diff 对比两份 .bin")
    ap.add_argument("--target", required=True, help="打印机真实 IP")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help="打印机端口，默认 9100")
    ap.add_argument("--listen", type=int, default=19100,
                    help="本地监听端口，默认 19100（避开 Windows 自带的 9100 客户端端口习惯）")
    ap.add_argument("--out", default="captures", help="输出目录，默认 captures")
    ap.add_argument("--label", default="", help="本次抓包的名称，如 white/black/duplex")
    ap.add_argument("--once", action="store_true", help="抓完一个连接就退出")
    ap.add_argument("--idle", type=float, default=2.0,
                    help="空闲多少秒判定作业结束，默认 2.0")
    ap.add_argument("--connect-timeout", type=float, default=5.0)
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind(("0.0.0.0", args.listen))
    except OSError as e:
        sys.stderr.write("监听 %d 失败: %s\n换一个 --listen 端口试试。\n" % (args.listen, e))
        return 1
    srv.listen(4)

    print("Port 9100 代理已启动")
    print("  本地监听 : 0.0.0.0:%d   <- Windows 驱动端口填这个" % args.listen)
    print("  转发到   : %s:%d" % (args.target, args.port))
    print("  输出目录 : %s" % os.path.abspath(args.out))
    print("  按 Ctrl+C 结束\n")

    index = 0
    try:
        while True:
            client, addr = srv.accept()
            index += 1
            print("[ .. ] #%d 来自 %s:%d，开始接收" % (index, addr[0], addr[1]))
            Session(args, index).run(client, addr)
            if args.once:
                break
    except KeyboardInterrupt:
        print("\n已停止。")
    finally:
        srv.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
