"""Pure-python PDF de-obfuscator for Standard security handler (V2/R3, RC4-128).

Builds a parsed object table with decrypted streams, then lets us pull text.
"""
import re, sys, zlib, hashlib, struct, pickle, os

PDF = r'E:\PDF库\声卡技术文档\CH32FV2x_V3xRM.pdf'
OUT = r'D:\mounriver-studio-projects\LittleTail\_pdfwork'

PAD = bytes([
    0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E, 0x56,
    0xFF, 0xFA, 0x01, 0x08, 0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80,
    0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A])


def rc4(key, data):
    S = list(range(256))
    j = 0
    klen = len(key)
    for i in range(256):
        j = (j + S[i] + key[i % klen]) & 0xFF
        S[i], S[j] = S[j], S[i]
    out = bytearray(len(data))
    i = j = 0
    for n, c in enumerate(data):
        i = (i + 1) & 0xFF
        j = (j + S[i]) & 0xFF
        S[i], S[j] = S[j], S[i]
        out[n] = c ^ S[(S[i] + S[j]) & 0xFF]
    return bytes(out)


def compute_key(pw, O, P, ID, R=3, length=128):
    n = length // 8
    h = hashlib.md5()
    h.update(pw + PAD[:32 - len(pw)])
    h.update(O[:32])
    h.update(struct.pack('<i', P))
    h.update(ID)
    key = h.digest()
    if R >= 3:
        for _ in range(50):
            key = hashlib.md5(key[:n]).digest()
    return key[:n]


def obj_key(key, num, gen):
    h = hashlib.md5()
    h.update(key)
    h.update(struct.pack('<I', num)[:3])
    h.update(struct.pack('<I', gen)[:2])
    return h.digest()[:min(len(key) + 5, 16)]


d = open(PDF, 'rb').read()

# ---- locate encryption params ----
enc = d[d.find(b'4792 0 obj'):]
enc = enc[:enc.find(b'endobj')]
mO = re.search(rb'/O\s*\((.*?)\)\s*/U', enc, re.S)
Oraw = mO.group(1)
# unescape PDF literal string
def unescape(s):
    out = bytearray(); i = 0
    while i < len(s):
        c = s[i]
        if c == 0x5C:  # backslash
            i += 1
            e = s[i]
            mp = {0x6E: 10, 0x72: 13, 0x74: 9, 0x62: 8, 0x66: 12}
            if e in mp: out.append(mp[e]); i += 1
            elif 0x30 <= e <= 0x37:
                j = i; oct_s = b''
                while j < len(s) and 0x30 <= s[j] <= 0x37 and len(oct_s) < 3:
                    oct_s += bytes([s[j]]); j += 1
                out.append(int(oct_s, 8) & 0xFF); i = j
            else: out.append(e); i += 1
        else:
            out.append(c); i += 1
    return bytes(out)

O = unescape(Oraw)
mP = re.search(rb'/P\s*(-?\d+)', enc)
P = int(mP.group(1))
mID = re.search(rb'/ID\s*\[\s*<([0-9A-Fa-f]+)>', d[d.rfind(b'trailer'):])
ID = bytes.fromhex(mID.group(1).decode())

print('O len', len(O), 'P', P, 'ID', ID.hex())

key = compute_key(b'', O, P, ID)
print('file key', key.hex())

# ---- parse xref ----
sx = d.rfind(b'startxref')
off = int(re.search(rb'(\d+)', d[sx + 9:]).group(1))
print('xref off', off, 'bytes there:', d[off:off + 20])

# Walk all xref sections (chained /Prev)
xref = {}
seen = set()
cur = off
while cur is not None and cur not in seen:
    seen.add(cur)
    seg = d[cur:cur + 400000]
    if not seg.startswith(b'xref'):
        print('WARN: not classic xref at', cur, seg[:30])
        break
    body = seg[4:]
    # find trailer
    tpos = body.find(b'trailer')
    entries = body[:tpos]
    for mm in re.finditer(rb'(\d+)\s+(\d+)\s*\r?\n', entries):
        start = int(mm.group(1)); cnt = int(mm.group(2))
        pos = mm.end()
        for k in range(cnt):
            line = entries[pos:pos + 20]
            f = line.split()
            off_s, gen_s, typ = f[0], f[1], f[2]
            if typ == b'n':
                onum = start + k
                if onum not in xref:
                    xref[onum] = int(off_s)
            pos += 20
    tr = d[cur + 4 + tpos: cur + 4 + tpos + 3000]
    mp = re.search(rb'/Prev\s+(\d+)', tr)
    cur = int(mp.group(1)) if mp else None

print('xref entries', len(xref), 'max obj', max(xref))

# ---- extract objects ----
objs = {}
for num, o in xref.items():
    m = re.match(rb'\s*%d\s+(\d+)\s+obj' % num, d[o:o + 40])
    if not m:
        continue
    gen = int(m.group(1))
    body_start = o + m.end()
    e = d.find(b'endobj', body_start)
    raw = d[body_start:e]
    objs[num] = (gen, raw)

print('objs parsed', len(objs))

# ---- decrypt streams ----
def decrypt_obj(num, gen, raw):
    k = obj_key(key, num, gen)
    # decrypt all literal strings + stream
    out = bytearray()
    i = 0
    n = len(raw)
    while i < n:
        c = raw[i]
        if c == 0x28 and (i == 0 or raw[i - 1] not in b'\\'):
            # literal string: find matching close (no nesting expected in this doc)
            depth = 1; j = i + 1; buf = bytearray()
            while j < n and depth:
                ch = raw[j]
                if ch == 0x5C:
                    buf.append(ch); buf.append(raw[j + 1]); j += 2; continue
                if ch == 0x28: depth += 1
                elif ch == 0x29:
                    depth -= 1
                    if depth == 0: break
                buf.append(ch); j += 1
            dec = rc4(k, bytes(buf))
            out += b'(' + dec.replace(b'\\', b'\\\\').replace(b'(', b'\\(').replace(b')', b'\\)') + b')'
            i = j + 1
        elif raw.startswith(b'stream', i):
            # find stream start
            s = i + 6
            if raw[s:s + 2] == b'\r\n': s += 2
            elif raw[s:s + 1] in (b'\n', b'\r'): s += 1
            e2 = raw.rfind(b'endstream')
            data = raw[s:e2]
            dec = rc4(k, data)
            out += b'stream\r\n' + dec + b'\r\nendstream'
            i = e2 + len(b'endstream')
        else:
            out.append(c); i += 1
    return bytes(out)

dec_objs = {}
for num, (gen, raw) in objs.items():
    dec_objs[num] = (gen, decrypt_obj(num, gen, raw))

with open(os.path.join(OUT, 'objs.pkl'), 'wb') as f:
    pickle.dump(dec_objs, f)
print('saved decrypted objects')

# ---- quick sanity: Info title ----
if 1 in dec_objs:
    print('INFO:', dec_objs[1][1][:400].decode('utf-8', 'replace'))
