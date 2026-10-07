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

for obj in (6, 115, 10, 39):
    gen, raw = dec_objs[obj]
    print('=' * 30, 'font obj', obj)
    print(raw[:1500].decode('latin-1'))
    mt = re.search(rb'/ToUnicode\s+(\d+)\s+\d+\s+R', raw)
    if mt:
        d = decompress(int(mt.group(1)))
        print('--- ToUnicode obj', mt.group(1).decode(), 'len', len(d) if d else None)
        if d:
            print(d[:1500].decode('latin-1'))
