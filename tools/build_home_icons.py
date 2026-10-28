#!/usr/bin/env python3
"""把主页图标原图切成 60x60, 并生成 RGB565 C 头文件。

源图:
  docs/icons/home-icons-source.png  音乐/故事/童谣 三宫格横向排列
  docs/icons/settings-source.png    设置单图
背景可以是透明或纯白。
输出:
  docs/icons/<name>.png       60x60 普通状态
  docs/icons/<name>-large.png 72x72 选中放大状态
  src/home_icons.h            RGB565 数组 (纸色背景已烘焙)
"""
from pathlib import Path
from PIL import Image
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "docs" / "icons" / "home-icons-source.png"
SETTINGS_SRC = ROOT / "docs" / "icons" / "settings-source.png"
OUT_DIR = ROOT / "docs" / "icons"
HEADER = ROOT / "src" / "home_icons.h"

SIZE = 60
SEL_SIZE = 72
PAPER = (255, 242, 176)
NAMES = ["music", "story", "rhyme", "settings"]
LABELS = ["HOME_ICON_MUSIC", "HOME_ICON_STORY", "HOME_ICON_RHYME", "HOME_ICON_SETTINGS"]


def content_mask(img: Image.Image) -> np.ndarray:
    """透明底用 alpha；白底用与白色差异。"""
    a = np.array(img)
    if a.shape[2] == 4 and a[:, :, 3].min() < 250:
        return a[:, :, 3] >= 16
    rgb = a[:, :, :3]
    return (rgb < 245).any(axis=2)


def split_icons(mask: np.ndarray):
    cols = np.where(mask.any(axis=0))[0]
    if len(cols) == 0:
        raise SystemExit("source image has no content")
    ranges = []
    start = prev = int(cols[0])
    for c in cols[1:]:
        c = int(c)
        if c - prev > 30:
            ranges.append((start, prev))
            start = c
        prev = c
    ranges.append((start, prev))
    if len(ranges) != 3:
        raise SystemExit(f"expected 3 icons, found {len(ranges)}: {ranges}")

    boxes = []
    for x0, x1 in ranges:
        rows = np.where(mask[:, x0:x1 + 1].any(axis=1))[0]
        boxes.append((x0, int(rows[0]), x1 + 1, int(rows[-1]) + 1))
    return boxes


def bbox_of(mask: np.ndarray):
    ys, xs = np.where(mask)
    if len(xs) == 0:
        raise SystemExit("source image has no content")
    return (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)


def rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def swap16(v: int) -> int:
    """LGFX_Sprite 的 16bit 画布按字节交换存储; pushImage 默认按 swap565 直接写入。"""
    return ((v & 0xFF) << 8) | (v >> 8)


def main():
    if not SRC.exists():
        raise SystemExit(f"missing source: {SRC}")
    if not SETTINGS_SRC.exists():
        raise SystemExit(f"missing source: {SETTINGS_SRC}")

    src = Image.open(SRC).convert("RGBA")
    jobs = list(zip(NAMES[:3], [src] * 3, split_icons(content_mask(src))))

    settings = Image.open(SETTINGS_SRC).convert("RGBA")
    jobs.append(("settings", settings, bbox_of(content_mask(settings))))
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    arrays = []
    for name, source, box in jobs:
        crop = source.crop(box)
        # 先在高分辨率下合成纸色底, 避免透明边缘在缩放时发黑
        bg = Image.new("RGBA", crop.size, PAPER + (255,))
        flat = Image.alpha_composite(bg, crop).convert("RGB")

        icon = flat.resize((SIZE, SIZE), Image.LANCZOS)
        icon.save(OUT_DIR / f"{name}.png", optimize=True)
        sel = flat.resize((SEL_SIZE, SEL_SIZE), Image.LANCZOS)
        sel.save(OUT_DIR / f"{name}-large.png", optimize=True)

        def values_of(im, size):
            px = im.load()
            return [swap16(rgb565(*px[x, y])) for y in range(size) for x in range(size)]

        arrays.append((values_of(icon, SIZE), values_of(sel, SEL_SIZE)))
        print(f"{name}: crop={box} size={box[2]-box[0]}x{box[3]-box[1]} "
              f"-> {SIZE}x{SIZE} / {SEL_SIZE}x{SEL_SIZE}")

    lines = [
        "/* 自动生成: tools/build_home_icons.py",
        " * 源图: docs/icons/home-icons-source.png + docs/icons/settings-source.png",
        " * 纸色背景已烘焙。普通 60x60, 选中放大 72x72。",
        " * 注意: 数据是 LGFX_Sprite 的 swap565 字节序, 不是标准 RGB565。",
        " * pushImage 默认 _swapBytes=false, 会把数组按 swap565 原样写入画布。 */",
        "#pragma once",
        "#include <stdint.h>",
        "",
    ]
    for label, (values, values_sel) in zip(LABELS, arrays):
        lines.append(f"static const uint16_t {label}[{SIZE * SIZE}] = {{")
        for i in range(0, len(values), 12):
            row = ", ".join(f"0x{v:04X}" for v in values[i:i + 12])
            lines.append(f"  {row},")
        lines.append("};")
        lines.append("")
        lines.append(f"static const uint16_t {label}_SEL[{SEL_SIZE * SEL_SIZE}] = {{")
        for i in range(0, len(values_sel), 12):
            row = ", ".join(f"0x{v:04X}" for v in values_sel[i:i + 12])
            lines.append(f"  {row},")
        lines.append("};")
        lines.append("")
    HEADER.write_text("\n".join(lines), encoding="utf-8")
    print(f"header: {HEADER}")


if __name__ == "__main__":
    main()
