"""TJ-aware text positioning: compute true x for each string segment."""
import re, sys, zlib, pickle, os
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, r'D:\mounriver-studio-projects\LittleTail\_pdfwork')
from dump2 import dec_objs, decompress, get_fonts_of_page, pages_list, TOKEN, unesc, decode

def font_widths(num):
    gen, raw = dec_objs[num]
    m = re.search(rb'/FirstChar\s+(\d+)', raw)
    first = int(m.group(1)) if m else 0
    mw = re.search(rb'/Widths\s*\[(.*?)\]', raw, re.S)
    ws = {}
    if mw:
        for i, v in enumerate(re.findall(rb'[-\d.]+', mw.group(1))):
            ws[first + i] = float(v)
    return ws

def page_segments(pno):
    pnum = pages_list[pno - 1]
    raw = dec_objs[pnum][1]
    fonts = get_fonts_of_page(raw)
    # width tables per resource name
    fwidths = {}
    for name, info in fonts.items():
        pass
    # rebuild name->objnum
    mr = re.search(rb'/Resources\s+(\d+)\s+\d+\s+R', raw)
    res = dec_objs[int(mr.group(1))][1] if mr else raw
    name2obj = {}
    pos = res.find(b'/Font')
    if pos >= 0:
        sub = res[pos:]
        start = sub.find(b'<<'); depth = 0; i = start
        while i < len(sub):
            if sub[i:i+2] == b'<<': depth += 1; i += 2; continue
            if sub[i:i+2] == b'>>':
                depth -= 1; i += 2
                if depth == 0: break
                continue
            i += 1
        body = sub[start+2:i-2]
        for fm in re.finditer(rb'/([^\s/]+)\s+(\d+)\s+\d+\s+R', body):
            name2obj[fm.group(1).decode()] = int(fm.group(2))
    mc = re.search(rb'/Contents\s+(\d+)\s+\d+\s+R', raw)
    data = decompress(int(mc.group(1)))
    cur_name = None; cur_font = None; size = 0.0
    tm = [1,0,0,1,0,0]; tlm = list(tm); leading = 0.0
    pen_x = 0.0
    seps = []
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
                if nm and nums:
                    cur_name = nm[-1][1:]; cur_font = fonts.get(cur_name); size = nums[-1]
            if op == 'Tm' and len(nums) >= 6:
                tm = list(nums[-6:]); tlm = list(tm)
                pen_x = tm[4]
            elif op in ('Td', 'TD') and len(nums) >= 2:
                tx, ty = nums[-2], nums[-1]
                if op == 'TD': leading = -ty
                tlm = [tlm[0],tlm[1],tlm[2],tlm[3], tlm[4]+tx*tlm[0]+ty*tlm[2], tlm[5]+tx*tlm[1]+ty*tlm[3]]
                tm = list(tlm); pen_x = tm[4]
            elif op == 'T*':
                tlm = [tlm[0],tlm[1],tlm[2],tlm[3], tlm[4]-leading*tlm[2], tlm[5]-leading*tlm[3]]
                tm = list(tlm); pen_x = tm[4]
            elif op == 'TL' and nums:
                leading = nums[-1]
            elif op in ('Tj', "'", '"'):
                if op == "'":
                    tlm = [tlm[0],tlm[1],tlm[2],tlm[3], tlm[4]-leading*tlm[2], tlm[5]-leading*tlm[3]]
                    tm = list(tlm); pen_x = tm[4]
                ss = [p[1] for p in pending if p[0] == 'str']
                if ss:
                    t = decode(ss[-1], cur_font)
                    if t.strip():
                        seps.append((round(tm[5],1), round(pen_x,1), t, cur_name))
                    o = name2obj.get(cur_name)
                    w = font_widths(o) if o else {}
                    adv = sum(w.get(c, 1000) for c in ss[-1]) / 1000.0 * size * abs(tm[0])
                    pen_x += adv
            elif op == 'TJ':
                parts = []
                for p in pending:
                    if p[0] == 'str':
                        parts.append(decode(p[1], cur_font))
                        o = name2obj.get(cur_name)
                        w = font_widths(o) if o else {}
                        adv = sum(w.get(c, 1000) for c in p[1]) / 1000.0 * size * abs(tm[0])
                        pen_x += adv
                    elif p[0] == 'num':
                        pen_x += -p[1] / 1000.0 * size * abs(tm[0])
                t = ''.join(parts)
                if t.strip():
                    seps.append((round(tm[5],1), round(seps[-0][1] if False else 0,1), t, cur_name))
                    seps[-1] = (round(tm[5],1), None, t, cur_name)
            pending = []
    return seps

segs = page_segments(153)
lines = {}
for y, x, t, fn in segs:
    lines.setdefault(y, []).append((x, t, fn))
print('=== PDF page 153, true segment positions ===')
for y in sorted(lines, reverse=True):
    if 60 <= y <= 260:
        print('y=%-7s | %s' % (y, '  '.join('%s@%s[%s]' % (t, x, fn) for x, t, fn in sorted(lines[y], key=lambda z: (z[0] is None, z[0])))))
