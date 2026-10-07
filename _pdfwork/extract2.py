"""Corrected full extraction: proper token typing (no delimiter-string collision)."""
import re, sys, zlib, pickle, os
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, r'D:\mounriver-studio-projects\LittleTail\_pdfwork')
from dump2 import (dec_objs, decompress, get_font, get_fonts_of_page, pages_list,
                   TOKEN, unesc, decode)

OUT = r'D:\mounriver-studio-projects\LittleTail\_pdfwork'

def extract_page_runs(pnum):
    gen, raw = dec_objs[pnum]
    fonts = get_fonts_of_page(raw)
    contents = []
    mc = re.search(rb'/Contents\s+(\d+)\s+\d+\s+R', raw)
    if mc:
        contents.append(int(mc.group(1)))
    else:
        mc = re.search(rb'/Contents\s*\[(.*?)\]', raw, re.S)
        if mc:
            for mm in re.finditer(rb'(\d+)\s+\d+\s+R', mc.group(1)):
                contents.append(int(mm.group(1)))
    runs = []
    for cnum in contents:
        data = decompress(cnum)
        if not data:
            continue
        cur_font = None
        tm = tlm = [1, 0, 0, 1, 0, 0]
        leading = 0.0
        pending = []
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
                nums = [p[1] for p in pending if p[0] == 'num']
                if op == 'Tf':
                    nm = [p[1] for p in pending if p[0] == 'name']
                    if nm: cur_font = fonts.get(nm[-1][1:])
                if op in ('Tj', "'", '"'):
                    ss = [p[1] for p in pending if p[0] == 'str']
                    if op == "'":
                        tlm = [tlm[0], tlm[1], tlm[2], tlm[3],
                               tlm[4] - leading * tlm[2], tlm[5] - leading * tlm[3]]
                        tm = list(tlm)
                    if ss:
                        t = decode(ss[-1], cur_font)
                        if t.strip():
                            runs.append((round(tm[5], 1), round(tm[4], 1), t))
                elif op == 'TJ':
                    parts = [decode(p[1], cur_font) for p in pending if p[0] == 'str']
                    t = ''.join(parts)
                    if t.strip():
                        runs.append((round(tm[5], 1), round(tm[4], 1), t))
                elif op == 'Tm' and len(nums) >= 6:
                    tm = list(nums[-6:]); tlm = list(tm)
                elif op in ('Td', 'TD') and len(nums) >= 2:
                    tx, ty = nums[-2], nums[-1]
                    if op == 'TD': leading = -ty
                    tlm = [tlm[0], tlm[1], tlm[2], tlm[3],
                           tlm[4] + tx * tlm[0] + ty * tlm[2],
                           tlm[5] + tx * tlm[1] + ty * tlm[3]]
                    tm = list(tlm)
                elif op == 'TL' and nums:
                    leading = nums[-1]
                elif op == 'T*':
                    tlm = [tlm[0], tlm[1], tlm[2], tlm[3],
                           tlm[4] - leading * tlm[2], tlm[5] - leading * tlm[3]]
                    tm = list(tlm)
                pending = []
    return runs

allpages = {}
for idx, pnum in enumerate(pages_list, 1):
    try:
        allpages[idx] = extract_page_runs(pnum)
    except Exception as ex:
        print('page', idx, 'ERR', ex)
        allpages[idx] = []

pickle.dump(allpages, open(os.path.join(OUT, 'pages2.pkl'), 'wb'))
with open(os.path.join(OUT, 'manual2.txt'), 'w', encoding='utf-8') as f:
    for idx in sorted(allpages):
        f.write('\n===== PDF_PAGE %d =====\n' % idx)
        lines = {}
        for y, x, t in allpages[idx]:
            lines.setdefault(y, []).append((x, t))
        for y in sorted(lines, reverse=True):
            f.write(''.join(s for _, s in sorted(lines[y])) + '\n')
print('pages', len(allpages), 'runs', sum(len(v) for v in allpages.values()))
print('sample p308 y586:', [t for y, x, t in allpages[308] if 580 <= y <= 592])
for kw in ['I2SDIV', 'CNDTR', 'DMA', 'I2S']:
    n = open(os.path.join(OUT, 'manual2.txt'), encoding='utf-8').read().count(kw)
    print(kw, n)
