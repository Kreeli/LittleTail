import re, pickle, sys, os
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
OUT = r'D:\mounriver-studio-projects\LittleTail\_pdfwork'
pages = pickle.load(open(os.path.join(OUT, 'pages2.pkl'), 'rb'))

# map line number -> pdf page for manual2.txt
pagemap = []   # (line_no, pdf_page)
with open(os.path.join(OUT, 'manual2.txt'), encoding='utf-8') as f:
    for i, line in enumerate(f, 1):
        m = re.match(r'===== PDF_PAGE (\d+) =====', line)
        if m:
            pagemap.append((i, int(m.group(1))))

def pdf_of_line(ln):
    p = None
    for l, pg in pagemap:
        if l <= ln: p = pg
        else: break
    return p

targets = [
    ('3.3.4 PLL时钟', 3485), ('3.3.5.8 I2S和RNG时钟', 3562),
    ('3.4.12 RCC_CFGR2', 4490), ('RCC_CFGR2 I2S3SRC/I2S2SRC', 4537),
    ('11.2.1 DMA通道处理', 8786), ('11.2.1 循环模式', 8824),
    ('11.2.2 可编程传输总大小', 8840), ('11.3.1 DMAx_INTFR', 9527),
    ('11.3.2 DMAx_INTFCR', 9567), ('11.3.3 DMAy_CFGRx', 9592),
    ('11.3.4 DMAy_CNTRx', 9645), ('11.3.4 注', 9659),
    ('11.3.8 DMA2_CNTRx', 9744), ('表11-6 DMA各通道外设映射', 9312),
    ('20.2.6 DMA (SPI)', 16810), ('20.3.1 I2S概述', 16882),
    ('20.3.2 支持的音频协议', 16904), ('20.3.2 DATAR/DMA 2次传输', 16914),
    ('20.3.3 时钟发生器', 17131), ('20.3.3 采样频率列表', 17170),
    ('20.3.3 公式 MCKOE=1', 17172), ('20.3.3 公式 MCKOE=0', 17175),
    ('20.3.4 I2S主模式', 17178), ('20.3.9 DMA功能', 17307),
    ('20.4.2 SPIx_CTLR2', 17436), ('20.4.8 I2S_CFGR', 17554),
    ('20.4.9 I2SPR', 17611), ('20.4.9 MCKOE/ODD/I2SDIV', 17620),
]
for name, ln in targets:
    p = pdf_of_line(ln)
    print('%-32s line %-6d PDF %-5s printed %s' % (name, ln, p, (p - 3) if p else None))

t = open(os.path.join(OUT, 'manual2.txt'), encoding='utf-8').read()
for kw in ['44.1', 'I2SDIV =', 'I2SDIV=', 'MCLK', '1024', '256*Fs', '256 × Fs']:
    print('occurrences of %-12r : %d' % (kw, t.count(kw)))
