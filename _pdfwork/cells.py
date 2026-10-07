"""Extract table cell rectangles/lines to assign labels to DMA channel columns."""
import re, sys, zlib, pickle, os
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
sys.path.insert(0, r'D:\mounriver-studio-projects\LittleTail\_pdfwork')
from dump2 import dec_objs, decompress, pages_list

TARGETS = {153: (60, 260), 151: (600, 820), 152: (540, 660)}

for pno, (lo, hi) in TARGETS.items():
    pnum = pages_list[pno - 1]
    raw = dec_objs[pnum][1]
    mc = re.search(rb'/Contents\s+(\d+)\s+\d+\s+R', raw)
    data = decompress(int(mc.group(1)))
    print('===== PDF page %d =====' % pno)
    # collect re ops
    rects = []
    for m in re.finditer(rb'([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+) re', data):
        x, y, w, h = [float(g) for g in m.groups()]
        rects.append((round(x, 1), round(y, 1), round(w, 1), round(h, 1)))
    # vertical lines
    vlines = []
    for m in re.finditer(rb'([-\d.]+) ([-\d.]+) m\s*([-\d.]+) ([-\d.]+) l\s*S', data):
        x1, y1, x2, y2 = [float(g) for g in m.groups()]
        if abs(x1 - x2) < 0.5 and abs(y1 - y2) > 3:
            vlines.append((round(x1, 1), round(min(y1, y2), 1), round(max(y1, y2), 1)))
    print('rects in band:', len([r for r in rects if lo <= r[1] <= hi]))
    xs = sorted(set(r[0] for r in rects if lo <= r[1] <= hi))
    print('rect x starts:', xs)
    ws = sorted(set(r[2] for r in rects if lo <= r[1] <= hi))
    print('rect widths:', ws)
    ys = sorted(set(r[1] for r in rects if lo <= r[1] <= hi))
    print('rect y starts:', ys[:20])
    vx = sorted(set(v[0] for v in vlines if v[1] <= hi and v[2] >= lo))
    print('vertical line x:', vx)
