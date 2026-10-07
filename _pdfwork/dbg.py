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
            cmap[int(a, 16)] = bytes.fromhex(b if len(b) % 2 == 0 else '0'+b).decode('utf-16-be', 'replace')
    return cmap

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
walk(2)

pnum = pages_list[307]
raw = dec_objs[pnum][1]
# extract.py style
mf = re.search(rb'/Font\s*<<(.*?)>>', raw, re.S)
print('extract.py style font body:')
print(mf.group(1).decode('latin-1') if mf else None)
fm1 = {}
if mf:
    for m in re.finditer(rb'/([^\s/]+)\s+(\d+)\s+\d+\s+R', mf.group(1)):
        fm1[m.group(1).decode()] = int(m.group(2))
print('map1', fm1)

# dump2 style
pos = raw.find(b'/Font')
sub = raw[pos:]
start = sub.find(b'<<'); depth = 0; i = start
while i < len(sub):
    if sub[i:i+2] == b'<<': depth += 1; i += 2; continue
    if sub[i:i+2] == b'>>':
        depth -= 1; i += 2
        if depth == 0: break
        continue
    i += 1
body = sub[start+2:i-2]
fm2 = {}
for m in re.finditer(rb'/([^\s/]+)\s+(\d+)\s+\d+\s+R', body):
    fm2[m.group(1).decode()] = int(m.group(2))
print('map2', fm2)
print('maps equal?', fm1 == fm2)
for k in set(fm1) | set(fm2):
    if fm1.get(k) != fm2.get(k):
        print('DIFF', k, fm1.get(k), fm2.get(k))

# now the run at x=236.2
mc = re.search(rb'/Contents\s+(\d+)\s+\d+\s+R', raw)
data = decompress(int(mc.group(1)))
# find the Tf/Tj sequence around 'In' etc: search for a Tf with F1+5 then TJ
i = data.find(b'/F1+5')
while i != -1 and i < len(data):
    seg = data[i:i+200]
    if b'Tf' in seg[:20]:
        print('--- F1+5 usage at', i)
        print(repr(seg[:150]))
        i = data.find(b'/F1+5', i+1)
        if i > 40000: break
    else:
        i = data.find(b'/F1+5', i+1)
