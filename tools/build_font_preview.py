"""Build an offline HTML specimen from real device font data and a system font.

Usage: python3 tools/build_font_preview.py
Requires Pillow, and the installed LovyanGFX dependency. Does not modify fonts.
"""
from pathlib import Path
import base64
import codecs
import json
import re
import struct
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
EFONT = ROOT / '.pio/libdeps/esp32-s3-devkitm-1/LovyanGFX/src/lgfx/Fonts/efont'
NOTO = Path('/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc')
SAMPLE = '小小故事盒月亮邮递员刷新列表音乐播放正在暂停音量设置亮度旋转方向均衡器故事分类全部歌曲白雪公主丑小鸭森林里的朋友下一首返回选择保存停止长按双击上下调整加载中请稍候清晰圆润边缘笔画中文对比测试清风明月夜听雨春夏秋冬你我他的和：，。…· /0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz%-'
CHARS = sorted(set(SAMPLE) | {chr(cp) for cp in range(32, 127)})

def pack(w, h, x, y, advance, alpha):
    assert len(alpha) == w * h
    return [w, h, x, y, advance, base64.b64encode(bytes(alpha)).decode()]

def vlw(path):
    data = path.read_bytes()
    count, _, _, _, ascent, descent = struct.unpack_from('>6I', data)
    pos, glyphs, levels = 24 + count * 28, {}, set()
    for i in range(count):
        cp, h, w, advance, dy, dx, _ = struct.unpack_from('>7i', data, 24 + i * 28)
        alpha = data[pos:pos + w * h]
        pos += w * h
        levels.update(alpha)
        if chr(cp) in CHARS:
            glyphs[chr(cp)] = pack(w, h, dx, -dy, advance, alpha)
    assert pos == len(data)
    return dict(glyphs=glyphs, ascent=ascent, descent=descent, levels=sorted(levels))

class Bits:
    def __init__(self, data, offset):
        self.data, self.bit = data, offset * 8
    def take(self, count, signed=False):
        n = 0
        for i in range(count):
            n |= ((self.data[self.bit // 8] >> (self.bit % 8)) & 1) << i
            self.bit += 1
        return n - (1 << (count - 1)) if signed else n

def efont(size):
    source = (EFONT / 'lgfx_efont_cn.c').read_text()
    body = re.search(r'const uint8_t lgfx_efont_cn_' + str(size) + r'\[\d+\]\s*=((?:\s*"(?:\\.|[^"\\])*")+);', source, re.S)[1]
    data = b''.join(codecs.decode(s, 'unicode_escape').encode('latin1') for s in re.findall(r'"((?:\\.|[^"\\])*)"', body))
    def u16(pos): return int.from_bytes(data[pos:pos + 2], 'big')
    def locate(cp):
        pos = 23
        if cp <= 255:
            pos += u16(19) if cp >= 97 else u16(17) if cp >= 65 else 0
            while data[pos + 1]:
                if data[pos] == cp: return pos + 2
                pos += data[pos + 1]
        else:
            pos += u16(21)
            lut = pos
            while True:
                pos += u16(lut)
                end = u16(lut + 2)
                lut += 4
                if end >= cp: break
            while u16(pos):
                if u16(pos) == cp: return pos + 3
                pos += data[pos + 2]
        return None
    glyphs = {}
    for ch in CHARS:
        pos = locate(ord(ch))
        if pos is None: continue
        bits = Bits(data, pos)
        w, h = bits.take(data[4]), bits.take(data[5])
        x, bearing = bits.take(data[6], True), bits.take(data[7], True)
        advance = bits.take(data[8], True)
        alpha = []
        while len(alpha) < w * h:
            zero, one = bits.take(data[2]), bits.take(data[3])
            while True:
                alpha.extend([0] * zero + [255] * one)
                repeat = bits.take(1)
                if not repeat: break
            assert len(alpha) <= w * h, (ch, size)
        glyphs[ch] = pack(w, h, x, -bearing - h, advance, alpha)
    return dict(glyphs=glyphs, levels=[0, 255])

def noto(size):
    font = ImageFont.truetype(str(NOTO), size, index=2)  # Simplified Chinese face
    glyphs, levels = {}, set()
    for ch in CHARS:
        x, y, right, bottom = font.getbbox(ch, anchor='ls')
        w, h = right - x, bottom - y
        image = Image.new('L', (w, h))
        if w and h: ImageDraw.Draw(image).text((-x, -y), ch, font=font, fill=255, anchor='ls')
        alpha = image.tobytes()
        levels.update(alpha)
        glyphs[ch] = pack(w, h, x, y, round(font.getlength(ch)), alpha)
    assert any(0 < value < 255 for value in levels)
    return dict(glyphs=glyphs, levels=sorted(levels))

fonts = {'current': {str(n): vlw(ROOT / f'fonts/ui{n}.vlw') for n in (14, 15, 16)},
         'pixel': {str(n): efont(n) for n in (14, 16)},
         'smooth': {str(n): noto(n) for n in (14, 16, 18)}}
for family in fonts.values():
    for face in family.values():
        assert all(ch in face['glyphs'] for ch in '小小故事盒刷新列表音乐播放0123456789')
license_text = (EFONT / 'COPYRIGHT.txt').read_text()
noto_license = Path('/usr/share/doc/fonts-noto-cjk/copyright').read_text()
payload = json.dumps(dict(fonts=fonts, charset=''.join(CHARS), licenses=license_text + '\n\n' + noto_license), ensure_ascii=False)
template = (ROOT / 'tools/font_preview.template.html').read_text()
output = ROOT / 'docs/font-comparison.html'
output.parent.mkdir(exist_ok=True)
output.write_text(template.replace('/* FONT_DATA */', payload.replace('</', '<\\/')))
print(f'Generated {output} ({output.stat().st_size:,} bytes); real VLW/efont glyphs and grayscale Noto specimens')
