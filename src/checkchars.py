import re, struct, sys, os

B = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(B, 'launcher.c'), encoding='utf-8').read()
d = open(os.path.join(B, 'atlas.bin'), 'rb').read()

face_off = struct.unpack('<I', d[4:8])[0]
nf = struct.unpack('<H', d[8:10])[0]
blob_off = struct.unpack('<I', d[24:28])[0]

glyph_cp = []
for i in range(nf):
    o = face_off + i * 12
    bo, ng, lhas = struct.unpack('<III', d[o:o + 12])
    for k in range(ng):
        rec = d[bo + k * 22: bo + k * 22 + 22]
        cp = struct.unpack('<I', rec[0:4])[0]
        glyph_cp.append(cp)
have = set(glyph_cp)

# every non-ascii literal in the source
lits = re.findall(r'"([^"\\\n]*)"', src)
need = {}
for s in lits:
    for ch in s:
        if ord(ch) > 127 and ch not in need:
            need[ch] = s
missing = sorted(set(ord(c) for c in need) - have)
missing = [chr(c) for c in missing]
print('faces:', nf, 'distinct cjk chars used:', len(need), 'missing:', len(missing))
for ch in missing:
    print('  MISSING %r U+%04X  first seen in: %s' % (ch, ord(ch), need[ch]))
if not missing:
    print('  all characters present')

