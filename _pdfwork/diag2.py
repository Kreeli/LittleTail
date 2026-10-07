"""Dump raw content-stream operators for a page, to see formula layout/graphics."""
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

print('--- true font objects ---')
cnt = 0
for num in sorted(dec_objs):
    gen, raw = dec_objs[num]
    if not re.search(rb'/Type\s*/Font(\s|/|>)', raw):
        continue
    cnt += 1
    sub = re.search(rb'/Subtype\s*/(\w+)', raw)
    tu = re.search(rb'/ToUnicode\s+(\d+)\s+\d+\s+R', raw)
    base = re.search(rb'/BaseFont\s*/([^\s/]+)', raw)
    info = 'obj %d sub=%s base=%s' % (num, sub.group(1).decode() if sub else '?', base.group(1).decode() if base else '?')
    if tu:
        d = decompress(int(tu.group(1)))
        if d:
            t = d.decode('latin-1')
            n1 = len(re.findall(r'<[0-9A-Fa-f]+>\s*<[0-9A-Fa-f]+>', t))
            info += ' cmap_pairs~%d' % n1
        else:
            info += ' TU_DECOMPRESS_FAIL'
    else:
        info += ' NO_TU'
    print(info)
print('total font objects', cnt)

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
print('pages', len(pages_list))

target = 308
pnum = pages_list[target - 1]
raw = dec_objs[pnum][1]
print('=== page obj', pnum, '===')
print(raw[:700].decode('latin-1'))
mc = re.search(rb'/Contents\s+(\d+)\s+\d+\s+R', raw)
cnum = int(mc.group(1))
data = decompress(cnum)
print('content obj', cnum, 'len', len(data))
txt = data.decode('latin-1')
m = re.search(r'I2SxCLK', txt)
print('--- context around first I2SxCLK ---')
a = max(0, m.start() - 1500)
print(txt[a:m.start() + 700])
