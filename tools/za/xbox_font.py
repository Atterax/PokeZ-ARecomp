# Makes fonts/nintendo_ext_003_xbox.ttf from your fonts/nintendo_ext_003.bfttf (tools/za/nca_font.py): the button
# glyphs relabelled for an Xbox controller. Controls map by position (runtime/hle/bindings.c: east = A), so the
# Switch's A is the Xbox pad's B and X is its Y: the circled A/B and X/Y glyphs (outline U+E0A0.., filled U+E0E0..)
# trade places. runtime/hle/pl.c loads it when the prompts are Xbox's. Needs fonttools.
# usage (from LASRecomp/): python tools/za/xbox_font.py
import io, sys
from fontTools.ttLib import TTFont

SRC, DST = 'fonts/nintendo_ext_003.bfttf', 'fonts/nintendo_ext_003_xbox.ttf'
KEY = bytes([0x49, 0x62, 0x18, 0x06])
SWAPS = [(0xE0A0, 0xE0A1), (0xE0A2, 0xE0A3), (0xE0E0, 0xE0E1), (0xE0E2, 0xE0E3)]

d = open(SRC, 'rb').read()
size = int.from_bytes(bytes(a ^ b for a, b in zip(d[4:8], KEY)), 'big')
ttf = bytes(d[8 + i] ^ KEY[i % 4] for i in range(size))
font = TTFont(io.BytesIO(ttf))
for table in font['cmap'].tables:
    if not table.isUnicode():
        continue
    for a, b in SWAPS:
        if a in table.cmap and b in table.cmap:
            table.cmap[a], table.cmap[b] = table.cmap[b], table.cmap[a]
font.save(DST)
print('wrote', DST)
