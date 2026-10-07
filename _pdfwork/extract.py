"""Extract page text from the decrypted CH32FV2x/V3x reference manual."""
import re, sys, zlib, pickle, os, json
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

OUT = r'D:\mounriver-studio-projects\LittleTail\_pdfwork'
dec_objs = pickle.load(open(os.path.join(OUT, 'objs.pkl'), 'rb'))

# ---------- helpers ----------
def get_stream(num):
    gen, raw = dec_objs[num]
    i = raw.find(b'stream')
    if i < 0:
        return None
    s = i + 6
    if raw[s:s + 2] == b'\r\n': s += 2
    elif raw[s:s + 1] in (b'\n', b'\r'): s += 1
    e = raw.rfind(b'endstream')
    return raw[s:e]

def decompress(num):
    data = get_stream(num)
    if data is None:
        return None
    try:
        return zlib.decompress(data)
    except Exception:
        try:
            return zlib.decompressobj().decompress(data)
        except Exception:
            return None

# sanity check on how many streams decompress
ok = bad = nostream = 0
for num in dec_objs:
    if b'stream' in dec_objs[num][1]:
        r = decompress(num)
        if r is None: bad += 1
        else: ok += 1
    else:
        nostream += 1
print(f'streams: ok={ok} bad={bad} nostream={nostream}')

# ---------- resolve indirect refs ----------
def resolve(tok):
    m = re.match(rb'(\d+)\s+(\d+)\s+R$', tok.strip())
    if m:
        return int(m.group(1))
    return None

# ---------- page tree ----------
root = None
for num, (gen, raw) in dec_objs.items():
    if b'/Type' in raw and b'/Catalog' in raw:
        root = num
        break
print('catalog obj', root)
cat = dec_objs[root][1]
m = re.search(rb'/Pages\s+(\d+)\s+\d+\s+R', cat)
pages_root = int(m.group(1))
print('pages root', pages_root)

pages = []
def walk(num, seen=None):
    gen, raw = dec_objs[num]
    if b'/Type' in raw and re.search(rb'/Type\s*/Pages', raw):
        mk = re.search(rb'/Kids\s*\[(.*?)\]', raw, re.S)
        if mk:
            for mm in re.finditer(rb'(\d+)\s+\d+\s+R', mk.group(1)):
                walk(int(mm.group(1)))
    elif re.search(rb'/Type\s*/Page\b', raw):
        pages.append(num)

walk(pages_root)
print('pages found', len(pages))

# ---------- ToUnicode CMaps ----------
def parse_cmap(data):
    txt = data.decode('latin-1')
    cmap = {}
    for blk in re.findall(r'beginbfchar(.*?)endbfchar', txt, re.S):
        for a, b in re.findall(r'<([0-9A-Fa-f]+)>\s*<([0-9A-Fa-f]+)>', blk):
            try:
                cmap[int(a, 16)] = bytes.fromhex(b if len(b) % 2 == 0 else '0' + b).decode('utf-16-be', 'replace')
            except Exception:
                pass
    for blk in re.findall(r'beginbfrange(.*?)endbfrange', txt, re.S):
        # <lo> <hi> <dst>  or  <lo> <hi> [ <d1> <d2> ... ]
        for mm in re.finditer(r'<([0-9A-Fa-f]+)>\s*<([0-9A-Fa-f]+)>\s*(\[.*?\]|<[0-9A-Fa-f]+>)', blk, re.S):
            lo = int(mm.group(1), 16); hi = int(mm.group(2), 16); dst = mm.group(3)
            if dst.startswith(b'['):
                items = re.findall(r'<([0-9A-Fa-f]+)>', dst.decode())
                for k, it in enumerate(items):
                    try:
                        cmap[lo + k] = bytes.fromhex(it if len(it) % 2 == 0 else '0' + it).decode('utf-16-be', 'replace')
                    except Exception:
                        pass
            else:
                base = dst[1:-1].decode()
                base_i = int(base, 16)
                width = len(base) // 4   # number of UTF-16 units
                for k in range(hi - lo + 1):
                    v = base_i + k
                    h = '%0*x' % (len(base), v)
                    try:
                        cmap[lo + k] = bytes.fromhex(h).decode('utf-16-be', 'replace')
                    except Exception:
                        pass
    return cmap

font_cache = {}
def get_font(num):
    if num in font_cache:
        return font_cache[num]
    gen, raw = dec_objs[num]
    info = {'raw': raw, 'cmap': None, 'two_byte': False}
    if re.search(rb'/Subtype\s*/Type0', raw):
        info['two_byte'] = True
    mt = re.search(rb'/ToUnicode\s+(\d+)\s+\d+\s+R', raw)
    if mt:
        d = decompress(int(mt.group(1)))
        if d:
            info['cmap'] = parse_cmap(d)
    font_cache[num] = info
    return info

# ---------- content stream text extraction ----------
TOKEN = re.compile(rb'''
    (?P<str>\((?:\\.|[^\\()]|\((?:\\.|[^\\()])*\))*\))
  | (?P<hex><[0-9A-Fa-f\s]*>)
  | (?P<arr>\[)
  | (?P<name>/[^\s/\[\]<>(){}]*)
  | (?P<num>[-+]?[\d.]+)
  | (?P<op>[A-Za-z'"*][A-Za-z0-9*'"]*)
  | (?P<other>\S)
''', re.X | re.S)

def unescape_pdf_string(s):
    out = bytearray(); i = 0
    while i < len(s):
        c = s[i]
        if c == 0x5C:
            i += 1
            if i >= len(s): break
            e = s[i]
            mp = {0x6E: 10, 0x72: 13, 0x74: 9, 0x62: 8, 0x66: 12, 0x28: 40, 0x29: 41, 0x5C: 92}
            if e in mp: out.append(mp[e]); i += 1
            elif 0x30 <= e <= 0x37:
                j = i; o = b''
                while j < len(s) and 0x30 <= s[j] <= 0x37 and len(o) < 3:
                    o += bytes([s[j]]); j += 1
                out.append(int(o, 8) & 0xFF); i = j
            elif e in (10, 13):
                i += 1
            else:
                out.append(e); i += 1
        else:
            out.append(c); i += 1
    return bytes(out)

def decode_with_font(data, font):
    if font and font['two_byte']:
        codes = [int.from_bytes(data[i:i + 2], 'big') for i in range(0, len(data) - 1, 2)]
    else:
        codes = list(data)
    if font and font['cmap']:
        return ''.join(font['cmap'].get(c, '') for c in codes)
    # fallback
    if font and font['two_byte']:
        return ''.join(chr(c) if 32 <= c < 0x3000 else '' for c in codes)
    return data.decode('latin-1', 'replace')

def extract_page(num):
    gen, raw = dec_objs[num]
    # resources -> font map
    fonts = {}
    mr = re.search(rb'/Resources\s+(\d+)\s+\d+\s+R', raw)
    res_raw = raw
    if mr:
        res_raw = dec_objs[int(mr.group(1))][1]
    mf = re.search(rb'/Font\s*<<(.*?)>>', res_raw, re.S)
    if not mf:
        mf2 = re.search(rb'/Font\s+(\d+)\s+\d+\s+R', res_raw)
        if mf2:
            fd = dec_objs[int(mf2.group(1))][1]
            mf = re.search(rb'<<(.*?)>>', fd, re.S)
    if mf:
        for fm in re.finditer(rb'/([^\s/]+)\s+(\d+)\s+\d+\s+R', mf.group(1)):
            fonts[fm.group(1).decode()] = get_font(int(fm.group(2)))

    # contents
    contents = []
    mc = re.search(rb'/Contents\s+(\d+)\s+\d+\s+R', raw)
    if mc:
        contents.append(int(mc.group(1)))
    else:
        mc = re.search(rb'/Contents\s*\[(.*?)\]', raw, re.S)
        if mc:
            for mm in re.finditer(rb'(\d+)\s+\d+\s+R', mc.group(1)):
                contents.append(int(mm.group(1)))

    runs = []   # (y, x, text)
    for cnum in contents:
        data = decompress(cnum)
        if not data:
            continue
        cur_font = None
        tm = [1, 0, 0, 1, 0, 0]
        tlm = [1, 0, 0, 1, 0, 0]
        leading = 0.0
        stack = []
        pending = []   # numbers/strings accumulated before operator
        in_text = False
        pos = 0
        for tk in TOKEN.finditer(data):
            kind = tk.lastgroup
            val = tk.group()
            if kind == 'num':
                try: pending.append(float(val))
                except Exception: pending.append(0.0)
            elif kind == 'str' or kind == 'hex':
                if kind == 'str':
                    pending.append(unescape_pdf_string(val[1:-1]))
                else:
                    h = re.sub(rb'\s', b'', val[1:-1])
                    if len(h) % 2: h += b'0'
                    pending.append(bytes.fromhex(h.decode()))
            elif kind == 'name':
                pending.append(val)
            elif kind == 'arr':
                pending.append(b'[')
            elif kind == 'other':
                if val in (b']', b'<', b'>'):
                    pending.append(val)
                else:
                    pending = []
            else:  # operator
                op = val.decode('latin-1')
                if op == 'Tf' and len(pending) >= 2:
                    nm = pending[-2]
                    if isinstance(nm, bytes) and nm.startswith(b'/'):
                        cur_font = fonts.get(nm[1:].decode())
                elif op == 'BT':
                    in_text = True; tm = [1, 0, 0, 1, 0, 0]; tlm = list(tm)
                elif op == 'ET':
                    in_text = False
                elif op == 'Tm' and len(pending) >= 6:
                    tm = [float(x) for x in pending[-6:]]; tlm = list(tm)
                elif op in ('Td', 'TD') and len(pending) >= 2:
                    tx, ty = float(pending[-2]), float(pending[-1])
                    if op == 'TD': leading = -ty
                    tlm = [tlm[0], tlm[1], tlm[2], tlm[3], tlm[4] + tx * tlm[0] + ty * tlm[2], tlm[5] + tx * tlm[1] + ty * tlm[3]]
                    tm = list(tlm)
                elif op == 'TL' and pending:
                    leading = float(pending[-1])
                elif op == 'T*':
                    tlm = [tlm[0], tlm[1], tlm[2], tlm[3], tlm[4] - leading * tlm[2], tlm[5] - leading * tlm[3]]
                    tm = list(tlm)
                elif op in ('Tj', "'", '"'):
                    if op == "'":
                        tlm = [tlm[0], tlm[1], tlm[2], tlm[3], tlm[4] - leading * tlm[2], tlm[5] - leading * tlm[3]]
                        tm = list(tlm)
                    ss = [p for p in pending if isinstance(p, bytes) and not p.startswith(b'/') and p not in (b'[', b']')]
                    if ss:
                        txt = decode_with_font(ss[-1], cur_font)
                        if txt.strip():
                            runs.append((round(tm[5], 1), round(tm[4], 1), txt))
                elif op == 'TJ':
                    parts = []
                    for p in pending:
                        if isinstance(p, bytes) and not p.startswith(b'/') and p not in (b'[', b']', b'<', b'>'):
                            parts.append(decode_with_font(p, cur_font))
                    txt = ''.join(parts)
                    if txt.strip():
                        runs.append((round(tm[5], 1), round(tm[4], 1), txt))
                pending = []
    return runs

# ---------- build and save all page text ----------
allpages = {}
for idx, pnum in enumerate(pages, 1):
    try:
        runs = extract_page(pnum)
    except Exception as ex:
        runs = []
        print('page', idx, 'ERR', ex)
    allpages[idx] = runs

with open(os.path.join(OUT, 'pages.pkl'), 'wb') as f:
    pickle.dump(allpages, f)

# also write plain text, lines grouped by y
with open(os.path.join(OUT, 'manual.txt'), 'w', encoding='utf-8') as f:
    for idx in sorted(allpages):
        f.write(f'\n===== PDF_PAGE {idx} =====\n')
        runs = allpages[idx]
        lines = {}
        for y, x, t in runs:
            lines.setdefault(y, []).append((x, t))
        for y in sorted(lines, reverse=True):
            segs = sorted(lines[y])
            f.write(''.join(s for _, s in segs) + '\n')

print('done. pages written:', len(allpages))
tot = sum(len(v) for v in allpages.values())
print('total text runs', tot)
print('sample page 30:', ''.join(t for _, _, t in allpages.get(30, []))[:200])
