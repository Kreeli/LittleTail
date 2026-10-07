"""Dump decoded operator listing (including graphics ops) for a page region."""
import re, sys, zlib, pickle, os
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
OUT = r'D:\mounriver-studio-projects\LittleTail\_pdfwork'
dec_objs = pickle.load(open(os.path.join(OUT, 'objs.pkl'), 'rb'))

def get_stream(num):
    gen, raw = dec_objs[num]
    i = raw.find(b'stream')
    if i < 0: return None
    s = i + 6
    if raw[s:s+2] == b'\r\n': s += 2
    elif raw[s:s+1] in (b'\n', b'\r'): s += 1
    return raw[s:raw.rfind(b'endstream')]

def decompress(num):
    d = get_stream(num)
    if d is None: return None
    try: return zlib.decompress(d)
    except Exception: return None

def parse_cmap(data):
    txt = data.decode('latin-1')
    cmap = {}
    for blk in re.findall(r'beginbfchar(.*?)endbfchar', txt, re.S):
        for a, b in re.findall(r'<([0-9A-Fa-f]+)>\s*<([0-9A-Fa-f]+)>', blk):
            try:
                h = b if len(b) % 2 == 0 else '0' + b
                cmap[int(a, 16)] = bytes.fromhex(h).decode('utf-16-be', 'replace')
            except Exception: pass
    for blk in re.findall(r'beginbfrange(.*?)endbfrange', txt, re.S):
        for mm in re.finditer(r'<([0-9A-Fa-f]+)>\s*<([0-9A-Fa-f]+)>\s*(\[.*?\]|<[0-9A-Fa-f]+>)', blk, re.S):
            lo = int(mm.group(1), 16); hi = int(mm.group(2), 16); dst = mm.group(3)
            if dst.startswith(b'['):
                items = re.findall(r'<([0-9A-Fa-f]+)>', dst.decode())
                for k, it in enumerate(items):
                    try: cmap[lo+k] = bytes.fromhex(it if len(it)%2==0 else '0'+it).decode('utf-16-be','replace')
                    except Exception: pass
            else:
                base = dst[1:-1].decode(); base_i = int(base, 16)
                for k in range(hi - lo + 1):
                    h = '%0*x' % (len(base), base_i + k)
                    try: cmap[lo+k] = bytes.fromhex(h).decode('utf-16-be','replace')
                    except Exception: pass
    return cmap

fcache = {}
def get_font(num):
    if num in fcache: return fcache[num]
    gen, raw = dec_objs[num]
    info = {'raw': raw, 'cmap': {}, 'two_byte': bool(re.search(rb'/Subtype\s*/Type0', raw))}
    mt = re.search(rb'/ToUnicode\s+(\d+)\s+\d+\s+R', raw)
    if mt:
        d = decompress(int(mt.group(1)))
        if d: info['cmap'] = parse_cmap(d)
    fcache[num] = info
    return info

def get_fonts_of_page(raw):
    fonts = {}
    mr = re.search(rb'/Resources\s+(\d+)\s+\d+\s+R', raw)
    res = dec_objs[int(mr.group(1))][1] if mr else raw
    pos = res.find(b'/Font')
    if pos < 0: return fonts
    # find the dict after /Font
    sub = res[pos:]
    # try inline dict with balanced braces
    if b'<<' in sub[:20]:
        start = sub.find(b'<<')
        depth = 0; i = start
        while i < len(sub):
            if sub[i:i+2] == b'<<': depth += 1; i += 2; continue
            if sub[i:i+2] == b'>>':
                depth -= 1; i += 2
                if depth == 0: break
                continue
            i += 1
        body = sub[start+2:i-2]
    else:
        m2 = re.search(rb'/Font\s+(\d+)\s+\d+\s+R', res)
        fd = dec_objs[int(m2.group(1))][1]
        body = fd[fd.find(b'<<')+2:fd.rfind(b'>>')]
    for fm in re.finditer(rb'/([^\s/]+)\s+(\d+)\s+\d+\s+R', body):
        fonts[fm.group(1).decode()] = get_font(int(fm.group(2)))
    return fonts

pages_root = 2
pages_list = []
def walk(num):
    gen, raw = dec_objs[num]
    if re.search(rb'/Type\s*/Pages', raw):
        mk = re.search(rb'/Kids\s*\[(.*?)\]', raw, re.S)
        if mk:
            for mm in re.finditer(rb'(\d+)\s+\d+\s+R', mk.group(1)):
                walk(int(mm.group(1)))
    elif re.search(rb'/Type\s*/Page\b', raw):
        pages_list.append(num)
walk(pages_root)

TOKEN = re.compile(rb'''
    (?P<str>\((?:\\.|[^\\()])*\))
  | (?P<hex><[0-9A-Fa-f\s]*>)
  | (?P<name>/[^\s/\[\]<>(){}]*)
  | (?P<num>[-+]?[\d.]+)
  | (?P<op>[A-Za-z'"*][A-Za-z0-9*'"]*)
  | (?P<other>\S)
''', re.X | re.S)

def unesc(s):
    out = bytearray(); i = 0
    while i < len(s):
        c = s[i]
        if c == 0x5C:
            i += 1
            if i >= len(s): break
            e = s[i]
            mp = {0x6E:10,0x72:13,0x74:9,0x62:8,0x66:12,0x28:40,0x29:41,0x5C:92}
            if e in mp: out.append(mp[e]); i += 1
            elif 0x30 <= e <= 0x37:
                j = i; o = b''
                while j < len(s) and 0x30 <= s[j] <= 0x37 and len(o) < 3:
                    o += bytes([s[j]]); j += 1
                out.append(int(o,8) & 0xFF); i = j
            else: out.append(e); i += 1
        else:
            out.append(c); i += 1
    return bytes(out)

def decode(data, font):
    if font and font['two_byte']:
        codes = [int.from_bytes(data[i:i+2],'big') for i in range(0,len(data)-1,2)]
    else:
        codes = list(data)
    cm = font['cmap'] if font else {}
    if cm:
        return ''.join(cm.get(c, '\ufffd') for c in codes)
    return data.decode('latin-1','replace')

target = 308
pnum = pages_list[target-1]
raw = dec_objs[pnum][1]
fonts = get_fonts_of_page(raw)
print('FONTS:', {k: v['raw'][:90].decode('latin-1') for k, v in fonts.items()})
mc = re.search(rb'/Contents\s+(\d+)\s+\d+\s+R', raw)
data = decompress(int(mc.group(1)))

cur_font = None
pending = []
listing = []
for tk in TOKEN.finditer(data):
    kind = tk.lastgroup; val = tk.group()
    if kind in ('num','name'): pending.append(val)
    elif kind == 'str': pending.append(unesc(val[1:-1]))
    elif kind == 'hex':
        h = re.sub(rb'\s', b'', val[1:-1])
        if len(h) % 2: h += b'0'
        pending.append(bytes.fromhex(h.decode()))
    elif kind == 'other':
        if val in (b'[', b']', b'<', b'>'): pending.append(val)
        else: pending = []
    else:
        op = val.decode('latin-1')
        if op == 'Tf' and len(pending) >= 2:
            nm = pending[-2]
            if nm.startswith(b'/'): cur_font = fonts.get(nm[1:].decode())
        if op in ('Tj','TJ',"'",'"'):
            ss = [p for p in pending if isinstance(p, bytes) and not p.startswith(b'/')]
            txt = ''.join(decode(p, cur_font) for p in ss)
            listing.append(('TEXT', txt, [p.hex() for p in ss], pending[-8:]))
        elif op in ('Tm','Td','TD','T*','re','m','l','f','S','W','cm','g','rg','scn','G','RG','J','j','w','d','i','gs','q','Q','BT','ET','TL','Tc','Tw','Tz','Ts','Tr'):
            listing.append((op, [p.decode('latin-1') if isinstance(p,bytes) else p for p in pending[:8]], None, None))
        pending = []

# find index of listing item whose text contains 'I2SxCLK'
idx = None
for i, it in enumerate(listing):
    if it[0]=='TEXT' and 'I2SxCLK' in it[1]:
        idx = i; break
print('found I2SxCLK at listing idx', idx, 'of', len(listing))
joined = ''.join(it[1] for it in listing if it[0] == 'TEXT')
print('--- joined page text (first 1200) ---')
print(joined[:1200])
print('--- occurrences of CLK ---')
for m in re.finditer('CLK', joined):
    print(repr(joined[max(0, m.start()-120):m.start()+120]))
    break
else:
    for it in listing[max(0,idx-30): idx+40]:
        if it[0]=='TEXT':
            print('TEXT %r  raw=%s' % (it[1], it[2]))
        else:
            print('%-4s %s' % (it[0], it[1]))
