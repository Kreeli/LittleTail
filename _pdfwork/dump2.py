"""Dump page operators with proper token typing, text positions and graphics ops."""
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
    info = {'cmap': {}, 'two_byte': bool(re.search(rb'/Subtype\s*/Type0', raw))}
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
    sub = res[pos:]
    if b'<<' in sub[:20]:
        start = sub.find(b'<<'); depth = 0; i = start
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
        return ''.join(cm.get(c, '?') for c in codes)
    return data.decode('latin-1','replace')

def dump_page(pageno, want=None, radius=40):
    pnum = pages_list[pageno-1]
    raw = dec_objs[pnum][1]
    fonts = get_fonts_of_page(raw)
    mc = re.search(rb'/Contents\s+(\d+)\s+\d+\s+R', raw)
    if not mc:
        print('no single contents'); return
    data = decompress(int(mc.group(1)))
    cur_font = None; cur_name = None
    pending = []          # list of ('num', float) / ('str', bytes) / ('name', str) / ('delim', str)
    listing = []
    tm = tlm = [1,0,0,1,0,0]; leading = 0.0
    ctm = [1,0,0,1,0,0]
    for tk in TOKEN.finditer(data):
        kind = tk.lastgroup; val = tk.group()
        if kind == 'num':
            try: pending.append(('num', float(val)))
            except Exception: pending.append(('num', 0.0))
        elif kind == 'str': pending.append(('str', unesc(val[1:-1])))
        elif kind == 'hex':
            h = re.sub(rb'\s', b'', val[1:-1])
            if len(h) % 2: h += b'0'
            pending.append(('str', bytes.fromhex(h.decode())))
        elif kind == 'name': pending.append(('name', val.decode('latin-1')))
        elif kind == 'other':
            if val in (b'[', b']'): pending.append(('delim', val.decode()))
            else: pending = []
        else:
            op = val.decode('latin-1')
            nums = [p[1] for p in pending if p[0]=='num']
            if op == 'Tf':
                nm = [p[1] for p in pending if p[0]=='name']
                if nm:
                    cur_name = nm[-1][1:]
                    cur_font = fonts.get(cur_name)
            if op in ('Tj', "'", '"'):
                ss = [p[1] for p in pending if p[0]=='str']
                if ss:
                    listing.append(('TEXT', decode(ss[-1], cur_font), (round(tm[4],1), round(tm[5],1)), cur_name))
            elif op == 'TJ':
                parts = [decode(p[1], cur_font) for p in pending if p[0]=='str']
                if parts:
                    listing.append(('TEXT', ''.join(parts), (round(tm[4],1), round(tm[5],1)), cur_name))
            elif op == 'Tm' and len(nums) >= 6:
                tm = list(nums[-6:]); tlm = list(tm)
            elif op in ('Td','TD') and len(nums) >= 2:
                tx, ty = nums[-2], nums[-1]
                if op == 'TD': leading = -ty
                tlm = [tlm[0],tlm[1],tlm[2],tlm[3], tlm[4]+tx*tlm[0]+ty*tlm[2], tlm[5]+tx*tlm[1]+ty*tlm[3]]
                tm = list(tlm)
            elif op == 'TL' and nums:
                leading = nums[-1]
            elif op == 'T*':
                tlm = [tlm[0],tlm[1],tlm[2],tlm[3], tlm[4]-leading*tlm[2], tlm[5]-leading*tlm[3]]
                tm = list(tlm)
            elif op == 'cm' and len(nums) >= 6:
                ctm = list(nums[-6:])
            elif op in ('re','m','l','f','F','f*','S','s','c','v','y','h','W','W*','n','g','rg','G','RG','k','K','w','J','j','d','i','sh','Do','gs','q','Q','BI','EI','sc','scn','cs','CS','B','B*','b','b*'):
                if op in ('re','m','l','f','F','f*','S','s','c','v','y','h','B','B*','b','b*'):
                    listing.append(('GFX', op, [round(x,2) for x in nums], None))
            pending = []
    j = ''.join(it[1] for it in listing if it[0]=='TEXT')
    if want:
        for i, it in enumerate(listing):
            if it[0]=='TEXT' and want in it[1]:
                print('###### page %d idx %d ######' % (pageno, i))
                for k in listing[max(0,i-radius): i+radius]:
                    if k[0]=='TEXT':
                        print('  TEXT y=%-7s x=%-8s font=%-6s %r' % (k[2][1], k[2][0], k[3], k[1]))
                    else:
                        print('  %-4s %-4s %s' % (k[0], k[1], k[2]))
                return
        print('pattern %r not found on page %d' % (want, pageno))
        print('joined:', j[:600])
        return
    print(j[:2000])

if __name__ == '__main__':
    mode = sys.argv[1] if len(sys.argv) > 1 else 'dump'
    pno = int(sys.argv[2]) if len(sys.argv) > 2 else 308
    want = sys.argv[3] if len(sys.argv) > 3 else None
    dump_page(pno, want)
