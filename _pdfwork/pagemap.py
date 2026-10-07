import re, pickle, sys, os
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
OUT = r'D:\mounriver-studio-projects\LittleTail\_pdfwork'
pages = pickle.load(open(os.path.join(OUT, 'pages2.pkl'), 'rb'))
txt = open(os.path.join(OUT, 'manual2.txt'), encoding='utf-8').read()

# per page, find trailer "V2.5 NNN"
ofs = {}
for idx, runs in pages.items():
    joined = ''.join(t for _, _, t in runs)
    m = re.findall(r'V2\.5\s*(\d+)', joined)
    if m:
        ofs[idx] = (int(m[-1]), idx - int(m[-1]))
vals = {}
for k, v in ofs.items():
    vals.setdefault(v[1], []).append(k)
print('offset histogram (printed = pdf - offset):')
for off, ks in sorted(vals.items(), key=lambda x: -len(x[1]))[:6]:
    print('  offset %d: %d pages, e.g. %s' % (off, len(ks), sorted(ks)[:8]))

def printed(pdfno):
    return ofs.get(pdfno, (None, None))[0]

# key locations
targets = {
    '20.3.3 时钟发生器': None, '20.3.4 I2S主模式': None, '20.4.9 SPI_I2S预分频': None,
    '20.3.9 DMA功能': None, '11.3.4': None, '11.2.2 可编程': None, '11.2.1': None,
    '3.3.5.8': None, '3.2.6': None, '20.2.6 DMA': None, '20.3.2 支持的音频协议': None,
}
for idx in sorted(pages):
    joined = ''.join(t for _, _, t in pages[idx])
    for t in targets:
        if t in joined and targets[t] is None:
            targets[t] = idx
for t, v in targets.items():
    print('%-28s PDF %-5s printed %s' % (t, v, printed(v) if v else None))
