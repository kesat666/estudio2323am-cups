#!/usr/bin/env python3
"""port9100_proxy 的离线自测：本机起假打印机 + 代理，验证转发与落盘。

不碰真机，可在任何机器上跑：
    python tools/selftest_proxy.py
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROXY = os.path.join(HERE, "port9100_proxy.py")
OUT = os.path.join(HERE, "_selftest_out")
FAKE_PORT = 19101
LISTEN = 19100

recv_box = {}


def fake_printer():
    """假打印机：收完数据后回一段 PJL 状态，模拟双向回读。"""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", FAKE_PORT))
    srv.listen(1)
    conn, _ = srv.accept()
    buf = bytearray()
    while True:
        d = conn.recv(65536)
        if not d:
            break
        buf.extend(d)
    recv_box["data"] = bytes(buf)
    conn.sendall(b'\x1b%-12345X@PJL INFO ID\r\n"TOSHIBA e-STUDIO2323AM"\r\n\x0c')
    time.sleep(0.2)
    conn.close()
    srv.close()


def main():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)

    t = threading.Thread(target=fake_printer, daemon=True)
    t.start()
    time.sleep(0.3)

    proc = subprocess.Popen(
        [sys.executable, PROXY, "--target", "127.0.0.1", "--port", str(FAKE_PORT),
         "--listen", str(LISTEN), "--out", OUT, "--label", "selftest",
         "--once", "--idle", "1.0"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.8)

    payload = b"\x1b%-12345X@PJL ENTER LANGUAGE=GDI\r\nGDIX" + bytes(range(0, 256)) * 4
    c = socket.create_connection(("127.0.0.1", LISTEN), timeout=5)
    c.sendall(payload)
    c.shutdown(socket.SHUT_WR)
    back = b""
    c.settimeout(3)
    try:
        while True:
            d = c.recv(4096)
            if not d:
                break
            back += d
    except socket.timeout:
        pass
    c.close()

    try:
        log = proc.communicate(timeout=15)[0]
    except subprocess.TimeoutExpired:
        proc.kill()
        log = proc.communicate()[0]
    t.join(timeout=3)

    fwd = os.path.join(OUT, "selftest.bin")
    bak = os.path.join(OUT, "selftest.back.bin")
    meta = os.path.join(OUT, "selftest.meta.json")

    checks = []
    checks.append(("代理正常退出", proc.returncode == 0))
    checks.append((".bin 已生成", os.path.isfile(fwd)))
    checks.append(("字节完全一致", os.path.isfile(fwd)
                   and open(fwd, "rb").read() == payload))
    checks.append(("数据已转发到打印机", recv_box.get("data") == payload))
    checks.append(("回读已落盘", os.path.isfile(bak)
                   and b"e-STUDIO2323AM" in open(bak, "rb").read()))
    ok_meta = False
    if os.path.isfile(meta):
        m = json.load(open(meta, encoding="utf-8"))
        ok_meta = (m["bytes_to_printer"] == len(payload)
                   and "PJL" in m["head_guess"]
                   and m["bytes_from_printer"] > 0)
    checks.append(("meta.json 字段正确", ok_meta))

    if os.path.isfile(meta):
        m = json.load(open(meta, encoding="utf-8"))
        print("  头部判断: %s" % m["head_guess"])
        print("  前 32 字节: %s" % (m["head_hexdump"].split("\n")[0] if m["head_hexdump"] else "-"))

    print("---- 代理输出 ----")
    print(log.strip())
    print("---- 自测结果 ----")
    fails = 0
    for name, ok in checks:
        print("  %s %s" % ("ok  " if ok else "FAIL", name))
        if not ok:
            fails += 1
    print("SELFTEST: %s" % ("PASSED" if fails == 0 else "FAILED (%d)" % fails))

    shutil.rmtree(OUT, ignore_errors=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
