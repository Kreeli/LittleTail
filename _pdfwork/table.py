import re, pickle, sys, os
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
OUT = r'D:\mounriver-studio-projects\LittleTail\_pdfwork'
pages = pickle.load(open(os.path.join(OUT, 'pages2.pkl'), 'rb'))

def show(pdfno, ymin, ymax):
    print('===== PDF page %d (printed %d), y %s..%s =====' % (pdfno, pdfno-3, ymin, ymax))
    lines = {}
    for y, x, t in pages[pdfno]:
        if ymin <= y <= ymax:
            lines.setdefault(y, []).append((x, t))
    for y in sorted(lines, reverse=True):
        segs = sorted(lines[y])
        print('y=%-7s | %s' % (y, '   '.join('%s@%s' % (t, x) for x, t in segs)))

# DMA mapping table 11-6 (printed 150) -> pdf 153
show(153, 80, 480)
