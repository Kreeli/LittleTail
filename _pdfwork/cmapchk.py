import re, sys, zlib, pickle, os
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
OUT = r'D:\mounriver-studio-projects\LittleTail\_pdfwork'
dec_objs = pickle.load(open(os.path.join(OUT, 'objs.pkl'), 'rb'))
pages = pickle.load(open(os.path.join(OUT, 'pages.pkl'), 'rb'))

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

def raw_cmap(data):
    txt = data.decode('latin-1')
    m = {}
    for blk in re.findall(r'beginbfchar(.*?)endbfchar', txt, re.S):
        for a, b in re.findall(r'<([0-9A-Fa-f]+)>\s*<([0-9A-Fa-f]+)>', blk):
            m[int(a, 16)] = b
    return m

for obj in (117, 8, 11, 40, 84, 152, 266, 467):
    gen, raw = dec_objs[obj]
    if b'/Type' not in raw or not re.search(rb'/Subtype/TrueType', raw):
        print('obj', obj, 'not a simple TT font'); continue
    mt = re.search(rb'/ToUnicode\s+(\d+)\s+\d+\s+R', raw)
    if not mt:
        print('obj %d: NO ToUnicode' % obj); continue
    d = decompress(int(mt.group(1)))
    m = raw_cmap(d)
    vals = list(m.values())
    dupes = {v: [k for k, vv in m.items() if vv == v] for v in set(vals) if vals.count(v) > 1}
    print('font obj %d: entries=%d distinct=%d dup_targets=%s' % (obj, len(m), len(set(vals)), {k: [hex(x) for x in v] for k, v in dupes.items()}))
    print('   codes range:', hex(min(m)), hex(max(m)))

# page 308 runs near y=586
print('--- pages.pkl page 308 runs y in [580,592] ---')
for y, x, t in pages[308]:
    if 580 <= y <= 592:
        print('  y=%s x=%s %r' % (y, x, t))
print('--- page 308 runs y in [330,340] ---')
for y, x, t in pages[308]:
    if 330 <= y <= 340:
        print('  y=%s x=%s %r' % (y, x, t))
