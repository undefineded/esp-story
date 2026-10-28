#!/usr/bin/env python3
"""生成主页预览图 (320x240, 2x 放大)。

使用 docs/icons 下由 build_home_icons.py 切出的 60x60 图标；
坐标与 src/ui.cpp 的 homeTile 保持一致，只用于设计评审。
设置图标仍为占位方块。
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

S = 2
W, H = 320 * S, 240 * S
ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
ICONS = DOCS / "icons"
OUT = DOCS / "home-preview.png"

PAPER   = "#fff2b0"
CORAL   = "#f05a56"
CORAL_D = "#d94343"
YELLOW  = "#ffc83d"
INK     = "#17324d"
TEXT    = "#000000"
WHITE   = "#ffffff"


def font(size, bold=False):
    name = "msyhbd.ttc" if bold else "msyh.ttc"
    return ImageFont.truetype(str(Path("C:/Windows/Fonts") / name), size, index=0)


F_LABEL = font(24)
F_TEXT  = font(32)


def sc(v):
    return int(round(v * S))


def rr(d, box, r, fill=None, outline=None, width=1):
    d.rounded_rectangle([sc(v) for v in box], radius=sc(r), fill=fill,
                        outline=outline, width=sc(width))


def text(d, xy, s, f, fill, anchor="lt"):
    d.text((sc(xy[0]), sc(xy[1])), s, font=f, fill=fill, anchor=anchor)


def screen():
    img = Image.new("RGB", (W, H), PAPER)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W, sc(30)], fill=CORAL)
    d.rectangle([0, sc(30), W, sc(32)], fill=CORAL_D)
    d.ellipse([sc(9), sc(5), sc(29), sc(25)], fill=YELLOW, outline=WHITE, width=sc(1))
    d.ellipse([sc(11), sc(7), sc(27), sc(23)], outline=WHITE, width=sc(1))
    er = sc(2)
    for cx in (13, 21):
        d.ellipse([sc(cx) - er, sc(10) - er, sc(cx) + er, sc(10) + er], fill=INK)
    d.rectangle([sc(13), sc(17), sc(25), sc(19)], fill=INK)
    text(d, (36, 7), "小小故事盒", F_TEXT, WHITE)
    text(d, (292, 7), "音量:10", F_TEXT, WHITE, anchor="rt")
    return img, d


def home_screen():
    img, d = screen()
    names = ["music", "story", "rhyme", "settings"]
    labels = ["音乐", "故事", "童谣", "设置"]
    selected = 0
    for i, (name, label) in enumerate(zip(names, labels)):
        x = 16 + i * (60 + 16)
        y = 86
        size = 72 if i == selected else 60
        ix = x - 6 if i == selected else x
        iy = y - 6 if i == selected else y
        suffix = "-large" if i == selected else ""
        icon = Image.open(ICONS / f"{name}{suffix}.png").convert("RGB")
        icon = icon.resize((size * S, size * S), Image.LANCZOS)
        img.paste(icon, (sc(ix), sc(iy)))
        text(d, (x + 30, y + 70), label, F_LABEL, TEXT, anchor="mt")
    text(d, (160, 218), "旋转选择   OK 进入", F_TEXT, TEXT, anchor="mt")
    return img


def main():
    DOCS.mkdir(parents=True, exist_ok=True)
    home_screen().save(OUT)
    print(OUT)


if __name__ == "__main__":
    main()
