import re, sys
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
d = open(r'E:\PDF库\声卡技术文档\CH32FV2x_V3xRM.pdf', 'rb').read()

m = re.search(rb'4792 0 obj(.{0,800}?)endobj', d, re.S)
print("=== Encrypt dict 4792 ===")
print(m.group(0).decode('latin-1') if m else 'not found')

print("=== Info obj 1 ===")
m2 = re.search(rb'\b1 0 obj(.{0,500}?)endobj', d, re.S)
print(m2.group(0).decode('latin-1') if m2 else 'nf')
