import re, pickle, sys, os
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
OUT = r'D:\mounriver-studio-projects\LittleTail\_pdfwork'
pages = pickle.load(open(os.path.join(OUT, 'pages2.pkl'), 'rb'))

def show(pdfno, ymin, ymax, label=''):
    print('===== PDF page %d (printed %d) %s =====' % (pdfno, pdfno-3, label))
    lines = {}
    for y, x, t in pages[pdfno]:
        if ymin <= y <= ymax:
            lines.setdefault(y, []).append((x, t))
    for y in sorted(lines, reverse=True):
        print('y=%-7s | %s' % (y, '  '.join('%s@%s' % (t, x) for x, t in sorted(lines[y]))))

show(151, 640, 800, 'DMA2 mapping table')
