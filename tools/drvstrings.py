#!/usr/bin/env python3
"""从 Toshiba GDI 驱动的 XML/DLL 中提取 PJL 相关词条，定位 MEDIASOURCE/DUPLEX/BINDING 的真实 token。"""
import re
import sys

KW = re.compile(r'(PJL|SIDES?|SIDED|BINDING|DUPLEX|MEDIASOURCE|MEDIATYPE|TRAY|CASSETTE|BYPASS|LONG|SHORT|MANUAL|STAPLE|COMMAND)', re.I)


def xml_hits(path):
    data = open(path, 'rb').read()
    text = None
    for enc in ('utf-8-sig', 'utf-16', 'gbk'):
        try:
            text = data.decode(enc)
            break
        except Exception:
            continue
    if text is None:
        return []
    out = []
    for ln in text.splitlines():
        if KW.search(ln):
            out.append(ln.strip()[:200])
    return out


def blob_strings(path):
    data = open(path, 'rb').read()
    seen = set()
    res = []
    for m in re.findall(rb'[\x20-\x7e]{4,}', data):
        s = m.decode('ascii', 'ignore')
        if KW.search(s) and s not in seen:
            seen.add(s)
            res.append('A ' + s[:140])
    for m in re.findall(rb'(?:[\x20-\x7e]\x00){4,}', data):
        s = m.decode('utf-16-le', 'ignore')
        if KW.search(s) and s not in seen:
            seen.add(s)
            res.append('W ' + s[:140])
    return res


for p in sys.argv[1:]:
    print('=' * 70)
    print(p)
    if p.lower().endswith(('.xml', '.txt', '.inf')):
        hits = xml_hits(p)
    else:
        hits = blob_strings(p)
    for h in hits:
        print('  ', h)
    print(f'  ({len(hits)} hits)')
