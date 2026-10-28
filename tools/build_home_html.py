"""Build an offline 320x240 home design preview using actual firmware glyphs."""
from pathlib import Path
import base64
import json
import struct

ROOT = Path(__file__).resolve().parents[1]
CHARS = set('小小故事盒音乐故事童谣设置旋转选择进入返回主页音乐舞台故事森林童谣花园工具小屋听见好心情打开新世界一起唱首歌准备好出发按下探索0123456789OK')

def face(size):
    data = (ROOT / f'fonts/ui{size}.vlw').read_bytes()
    count = struct.unpack_from('>I', data)[0]
    pos = 24 + count * 28
    glyphs = {}
    for i in range(count):
        cp, h, w, advance, dy, dx, _ = struct.unpack_from('>7i', data, 24 + i * 28)
        alpha = data[pos:pos + w * h]
        pos += w * h
        if chr(cp) in CHARS:
            glyphs[chr(cp)] = [w, h, advance, dy, dx, base64.b64encode(alpha).decode()]
    return glyphs

payload = dict(fonts={str(s): face(s) for s in (11, 14, 16)}, icons={
    name: 'data:image/png;base64,' + base64.b64encode((ROOT / f'docs/icons/{name}.png').read_bytes()).decode()
    for name in ('music', 'story', 'rhyme', 'settings')})
payload['scenes'] = 'data:image/png;base64,' + base64.b64encode((ROOT / 'docs/icons/home-scenes.png').read_bytes()).decode()
template = (ROOT / 'tools/home_preview.template.html').read_text()
out = ROOT / 'docs/home-preview.html'
out.write_text(template.replace('/* PAYLOAD */', json.dumps(payload, ensure_ascii=False)), encoding='utf-8')
print(out)
