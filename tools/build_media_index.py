#!/usr/bin/env python3
"""扫描 SD 卡的 /music 和 /story，生成一份人类可读的媒体索引。

扫描规则与固件 components/media/media_library.cpp 保持一致：
  /music  第一层目录就是分类，分类内部递归收集 .mp3；根目录散放的 .mp3 归入「全部歌曲」。
  /story  递归扫描：同一目录里同时有 .mp3 和 .lrc → 一个图文故事（标题取目录名）；
          否则该目录下的每个 .mp3 都是一个音频故事（标题取文件名）。
          故事分类取 /story 的第一层目录名。

用法：
  python tools/build_media_index.py --music D:\\music --story D:\\story --out-dir D:\\
输出：
  媒体索引.md、媒体索引.csv
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

# 固件上限默认值，见 components/media/include/media_library.h；
# 运行时会直接从该头文件读取，改了固件不用再改这里。
LIMITS = {"tracks": 2000, "categories": 32, "stories": 1024}
LIMIT_HEADER = Path(__file__).resolve().parents[1] / "components" / "media" / "include" / "media_library.h"
RECOMMENDED_RATE = 44100
IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}


def load_limits() -> dict:
    limits = dict(LIMITS)
    try:
        text = LIMIT_HEADER.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return limits
    for key, symbol in (("tracks", "kMaxTracks"), ("categories", "kMaxCategories"),
                        ("stories", "kMaxStories")):
        match = re.search(rf"\b{symbol}\s*=\s*(\d+)", text)
        if match:
            limits[key] = int(match.group(1))
    return limits


def natkey(text: str):
    """自然排序：数字按数值比较，其余按字符串。"""
    return [int(part) if part.isdigit() else part.casefold()
            for part in re.split(r"(\d+)", text)]


def fmt_duration(seconds: float | None) -> str:
    if seconds is None or seconds <= 0:
        return "--:--"
    seconds = int(round(seconds))
    hours, rest = divmod(seconds, 3600)
    minutes, secs = divmod(rest, 60)
    if hours:
        return f"{hours}:{minutes:02d}:{secs:02d}"
    return f"{minutes}:{secs:02d}"


def fmt_size(num_bytes: int) -> str:
    return f"{num_bytes / 1024 / 1024:.2f} MB"


@dataclass
class Media:
    path: Path
    rel: str
    title: str
    size: int
    duration: float | None = None
    bitrate: int | None = None
    sample_rate: int | None = None
    channels: int | None = None
    codec: str | None = None
    fingerprint: str = ""
    error: str = ""


@dataclass
class Category:
    name: str
    medias: list[Media] = field(default_factory=list)

    @property
    def total_size(self) -> int:
        return sum(m.size for m in self.medias)

    @property
    def total_duration(self) -> float:
        return sum(m.duration or 0 for m in self.medias)


def ffprobe(path: Path) -> dict:
    cmd = [
        "ffprobe", "-v", "error", "-print_format", "json",
        "-show_format", "-show_streams", str(path),
    ]
    try:
        out = subprocess.run(cmd, capture_output=True, check=False, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"error": str(exc)}
    if out.returncode != 0:
        return {"error": out.stderr.decode("utf-8", "replace").strip()[:200]}
    try:
        info = json.loads(out.stdout.decode("utf-8", "replace") or "{}")
    except json.JSONDecodeError as exc:
        return {"error": f"ffprobe json: {exc}"}
    audio = next((s for s in info.get("streams", []) if s.get("codec_type") == "audio"), {})
    fmt = info.get("format", {})
    duration = fmt.get("duration") or audio.get("duration")
    bitrate = fmt.get("bit_rate") or audio.get("bit_rate")
    return {
        "duration": float(duration) if duration else None,
        "bitrate": int(bitrate) if bitrate else None,
        "sample_rate": int(audio["sample_rate"]) if audio.get("sample_rate") else None,
        "channels": int(audio["channels"]) if audio.get("channels") else None,
        "codec": audio.get("codec_name"),
    }


def probe_all(medias: list[Media], jobs: int = 8) -> None:
    def work(media: Media) -> None:
        info = ffprobe(media.path)
        try:
            with media.path.open("rb") as handle:
                head = handle.read(128 * 1024)
            import hashlib
            media.fingerprint = hashlib.md5(head).hexdigest()
        except OSError as exc:
            media.error = media.error or str(exc)
        if info.get("error"):
            media.error = media.error or info["error"]
            return
        media.duration = info.get("duration")
        media.bitrate = info.get("bitrate")
        media.sample_rate = info.get("sample_rate")
        media.channels = info.get("channels")
        media.codec = info.get("codec")

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(work, medias))


def mp3_files(root: Path):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort(key=natkey)
        for name in sorted(filenames, key=natkey):
            if name.lower().endswith(".mp3"):
                yield Path(dirpath) / name


def scan_music(root: Path) -> list[Category]:
    categories: list[Category] = []
    loose = Category(name="全部歌曲（根目录散曲）")
    for entry in sorted(root.iterdir(), key=lambda p: natkey(p.name)):
        if entry.is_dir():
            category = Category(name=entry.name)
            for path in mp3_files(entry):
                category.medias.append(Media(
                    path=path,
                    rel=str(path.relative_to(root)).replace("\\", "/"),
                    title=path.stem,
                    size=path.stat().st_size,
                ))
            if category.medias:
                categories.append(category)
        elif entry.suffix.lower() == ".mp3":
            loose.medias.append(Media(
                path=entry,
                rel=entry.name,
                title=entry.stem,
                size=entry.stat().st_size,
            ))
    if loose.medias:
        categories.append(loose)
    return categories


def lrc_images(lrc: Path) -> list[str]:
    """从 timeline.lrc 里取出被引用的图片名。"""
    names = []
    try:
        text = lrc.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return names
    for line in text.splitlines():
        match = re.match(r"\[[^\]]*\]\s*([^|\]]+)", line)
        if not match:
            continue
        candidate = match.group(1).strip()
        if re.search(r"\.(jpg|jpeg|png|bmp|webp)$", candidate, re.I):
            names.append(candidate)
    return names


def is_lfs_pointer(path: Path) -> bool:
    try:
        if path.stat().st_size > 1024:
            return False
        head = path.read_bytes()[:40]
    except OSError:
        return False
    return head.startswith(b"version https://")


def scan_story(root: Path) -> tuple[list[Category], list[dict]]:
    """返回 (分类, 图文故事检查结果)。"""
    categories: dict[str, Category] = {}
    illustrated: list[dict] = []

    def add(media: Media, category_name: str) -> None:
        categories.setdefault(category_name, Category(name=category_name)).medias.append(media)

    def visit(folder: Path) -> None:
        try:
            children = sorted(folder.iterdir(), key=lambda p: natkey(p.name))
        except OSError:
            return
        files = [p for p in children if p.is_file()]
        mp3s = [p for p in files if p.suffix.lower() == ".mp3"]
        lrcs = [p for p in files if p.suffix.lower() == ".lrc"]
        rel_dir = folder.relative_to(root)
        parts = rel_dir.parts
        category_name = parts[0] if parts else "（根目录）"

        if mp3s and lrcs:
            audio = mp3s[0]
            media = Media(path=audio, rel=str(audio.relative_to(root)).replace("\\", "/"),
                          title=folder.name, size=audio.stat().st_size)
            add(media, category_name)
            referenced = lrc_images(lrcs[0])
            images = [p for p in files if p.suffix.lower() in IMAGE_EXTS]
            lower_actual = {p.name.casefold(): p.name for p in images}
            missing = [name for name in referenced if name.casefold() not in lower_actual]
            pointer = sorted(p.name for p in images if is_lfs_pointer(p))
            illustrated.append({
                "category": category_name,
                "title": folder.name,
                "rel": str(rel_dir).replace("\\", "/"),
                "images": len(images),
                "referenced": len(referenced),
                "missing": missing,
                "lfs": pointer,
                "exts": sorted({p.suffix.lower() for p in images}),
                "refexts": sorted({Path(name).suffix.lower() for name in referenced}),
            })
        elif mp3s:
            for path in mp3s:
                add(Media(path=path, rel=str(path.relative_to(root)).replace("\\", "/"),
                          title=path.stem, size=path.stat().st_size), category_name)

        for child in children:
            if child.is_dir():
                visit(child)

    if root.is_dir():
        visit(root)
    return [categories[name] for name in sorted(categories, key=natkey)], illustrated


def duplicate_groups(medias: list[Media]) -> list[list[Media]]:
    groups: dict[tuple[str, int], list[Media]] = {}
    for media in medias:
        groups.setdefault((media.title.casefold(), media.size), []).append(media)
    return [group for group in groups.values() if len(group) > 1]


def content_groups(medias: list[Media]) -> list[list[Media]]:
    """大小 + 前 128 KiB 指纹相同的文件，视为疑似同一首歌的副本。"""
    groups: dict[tuple[int, str], list[Media]] = {}
    for media in medias:
        if media.fingerprint:
            groups.setdefault((media.size, media.fingerprint), []).append(media)
    return [group for group in groups.values() if len(group) > 1]


def format_flags(media: Media) -> list[str]:
    flags = []
    if media.error:
        flags.append("读取失败")
        return flags
    if media.sample_rate and media.sample_rate != RECOMMENDED_RATE:
        flags.append(f"{media.sample_rate / 1000:g} kHz")
    if media.channels and media.channels != 2:
        flags.append("单声道" if media.channels == 1 else f"{media.channels} 声道")
    if media.bitrate and media.bitrate < 96000:
        flags.append("码率偏低")
    return flags


def table(rows: list[list[str]], header: list[str]) -> list[str]:
    lines = ["| " + " | ".join(header) + " |",
             "| " + " | ".join("---" for _ in header) + " |"]
    lines += ["| " + " | ".join(row) + " |" for row in rows]
    return lines


def build_markdown(music: list[Category], stories: list[Category], illustrated: list[dict],
                   args: argparse.Namespace) -> str:
    all_tracks = [m for c in music for m in c.medias]
    all_stories = [m for c in stories for m in c.medias]
    now = datetime.now().strftime("%Y-%m-%d %H:%M")

    out: list[str] = []
    out.append("# SD 卡媒体索引")
    out.append("")
    out.append(f"生成时间：{now} ｜ 扫描根目录：`{args.music}`、`{args.story}` ｜ "
               f"生成工具：`tools/build_media_index.py`")
    out.append("")
    out.append("统计口径与固件一致：`/music` 第一层目录为分类、分类内递归收歌；"
               "`/story` 中「同目录同时有 mp3 和 lrc」算一个图文故事，否则每个 mp3 算一个音频故事，"
               "分类取 `/story` 第一层目录名。时长/码率由 ffprobe 读取。")
    out.append("")

    out.append("## 1. 总览")
    out.append("")
    out.append(f"- 音乐：**{len(all_tracks)} 首**，{len(music)} 个分类，"
               f"{fmt_duration(sum(m.duration or 0 for m in all_tracks))}，"
               f"{sum(m.size for m in all_tracks) / 1024 / 1024:.1f} MB")
    out.append(f"- 故事：**{len(all_stories)} 条**，{len(stories)} 个分类，"
               f"{fmt_duration(sum(m.duration or 0 for m in all_stories))}，"
               f"{sum(m.size for m in all_stories) / 1024 / 1024:.1f} MB")
    usage = shutil.disk_usage(args.music)
    out.append(f"- 卡容量：已用 {usage.used / 1024 ** 3:.1f} GB，剩余 {usage.free / 1024 ** 3:.1f} GB "
               f"（含 `/video` 等本索引未统计的内容）")
    out.append("")
    out.append("### 分类一览")
    out.append("")
    rows = []
    for label, cats in (("音乐", music), ("故事", stories)):
        for cat in cats:
            rows.append([label, cat.name, str(len(cat.medias)),
                         fmt_duration(cat.total_duration), f"{cat.total_size / 1024 / 1024:.1f} MB"])
    out += table(rows, ["类型", "分类（SD 卡目录）", "数量", "总时长", "占用"])
    out.append("")

    out.append("## 2. 固件上限对照")
    out.append("")
    rows = [
        ["音乐曲目", f"{len(all_tracks)} / {LIMITS['tracks']}",
         "OK" if len(all_tracks) <= LIMITS["tracks"] else "超限"],
        ["音乐分类", f"{len(music)} / {LIMITS['categories']}",
         "OK" if len(music) <= LIMITS["categories"] else "超限"],
        ["故事条目", f"{len(all_stories)} / {LIMITS['stories']}",
         "OK" if len(all_stories) <= LIMITS["stories"] else "**超限**"],
    ]
    out += table(rows, ["项目", "当前 / 上限", "结论"])
    out.append("")
    if len(all_stories) > LIMITS["stories"]:
        out.append(f"> **故事数量超出固件上限**：扫描到 {len(all_stories)} 条，"
                   f"`kMaxStories` 只有 {LIMITS['stories']}。`Library::scan` 在 `addStory` 里一旦超出就返回 "
                   "`LimitExceeded`，整次扫描作废、不发布新目录——冷启动后音乐页和故事页都会是空的，"
                   "有旧缓存时则一直显示旧内容。可以把 `components/media/include/media_library.h` 的 "
                   f"`kMaxStories` 提到 {1 << (len(all_stories).bit_length())}，"
                   "或把故事拆到第二张卡 / 删掉一部分。")
        out.append("")
    if len(all_tracks) > LIMITS["tracks"] or len(music) > LIMITS["categories"]:
        out.append("> 音乐数量或分类数超出上限，扫描同样会失败，需要先减量。")
        out.append("")

    # 图文故事检查
    if illustrated:
        out.append("## 3. 图文故事自检")
        out.append("")
        rows = []
        broken: list[dict] = []
        for item in illustrated:
            problems = []
            if item["lfs"]:
                problems.append(f"{len(item['lfs'])}/{item['images']} 张图是 Git LFS 指针（131 字节文本，未取回真实图片）")
            if item["missing"]:
                problems.append(f"lrc 引用的 {len(item['missing'])}/{item['referenced']} 张图不存在")
            if set(item["refexts"]) - set(item["exts"]):
                problems.append(f"扩展名不符：lrc 写 {'/'.join(item['refexts'])}，目录里是 "
                                f"{'/'.join(item['exts']) or '无图'}")
            if item["images"] != item["referenced"] and not item["missing"]:
                problems.append(f"图片 {item['images']} 张、lrc 引用 {item['referenced']} 条，数量不一致")
            if problems:
                broken.append(item)
            rows.append([item["category"], item["title"], str(item["images"]),
                         str(item["referenced"]), "；".join(problems) or "OK"])
        out += table(rows, ["分类", "故事", "图片数", "lrc 引用数", "检查结果"])
        out.append("")
        if broken:
            out.append("> 标了问题的图文故事在设备上会「有声音、没插画」：时间轴按文件名去取图，"
                       "取不到就只放音频。`timeline.lrc` 里的文件名、扩展名、大小写必须和磁盘上的文件一一对应；"
                       "Git LFS 指针文件只有 131 字节左右（内容是一行 `version https://...`），"
                       "需要 `git lfs pull` 取回真实图片。")
            out.append("")

    out.append("## 4. 音乐清单")
    out.append("")
    for cat in music:
        out.append(f"### {cat.name}（{len(cat.medias)} 首，{fmt_duration(cat.total_duration)}，"
                   f"{cat.total_size / 1024 / 1024:.1f} MB）")
        out.append("")
        rows = []
        for index, media in enumerate(cat.medias, 1):
            flags = format_flags(media)
            rows.append([str(index), media.title, fmt_duration(media.duration),
                         f"{media.size / 1024 / 1024:.1f} MB",
                         f"{media.bitrate // 1000} kbps" if media.bitrate else "--",
                         "；".join(flags)])
        out += table(rows, ["#", "曲目（= 文件名）", "时长", "大小", "码率", "提示"])
        out.append("")

    out.append("## 5. 故事清单")
    out.append("")
    for cat in stories:
        out.append(f"### {cat.name}（{len(cat.medias)} 条，{fmt_duration(cat.total_duration)}，"
                   f"{cat.total_size / 1024 / 1024:.1f} MB）")
        out.append("")
        rows = []
        for index, media in enumerate(cat.medias, 1):
            rows.append([str(index), media.title, fmt_duration(media.duration),
                         f"{media.size / 1024 / 1024:.1f} MB"])
        out += table(rows, ["#", "故事（图文故事取目录名，音频故事取文件名）", "时长", "大小"])
        out.append("")

    # 格式与重复
    odd = [m for m in all_tracks + all_stories if format_flags(m)]
    out.append("## 6. 可能需要处理的条目")
    out.append("")
    if odd:
        out.append(f"格式不符合「44.1 kHz / 立体声 / ≥96 kbps」的音频共 **{len(odd)}** 条：")
        out.append("")
        rows = []
        for media in odd[:60]:
            rows.append([media.rel, "；".join(format_flags(media))])
        out += table(rows, ["路径（相对 /music 或 /story）", "问题"])
        if len(odd) > 60:
            out.append("")
            out.append(f"（只列前 60 条，共 {len(odd)} 条。）")
        out.append("")

    dupes = duplicate_groups(all_tracks + all_stories)
    if dupes:
        out.append(f"同名同大小的重复项 **{len(dupes)}** 组（同名不同大小视为不同文件，不列入）：")
        out.append("")
        rows = []
        for group in sorted(dupes, key=lambda g: (-len(g), natkey(g[0].title))):
            rows.append([group[0].title, str(len(group)),
                         " ⇄ ".join(m.rel for m in group)])
        out += table(rows, ["标题", "份数", "位置"])
        out.append("")

    copies = content_groups(all_tracks + all_stories)
    if copies:
        out.append(f"内容相同的副本 **{len(copies)}** 组（大小 + 前 128 KiB 指纹一致，标题可能不同）：")
        out.append("")
        rows = []
        for group in sorted(copies, key=lambda g: (-len(g), natkey(g[0].rel))):
            rows.append([group[0].title, f"{group[0].duration and fmt_duration(group[0].duration) or '--'}",
                         " ⇄ ".join(m.rel for m in group)])
        out += table(rows, ["标题", "时长", "位置"])
        out.append("")

    if not odd and not dupes and not copies:
        out.append("没有发现格式异常、重复或缺失的条目。")
        out.append("")

    out.append("## 7. 说明")
    out.append("")
    out.append("- 设备不在每次启动全盘扫描：优先读卡根的 `/.esp-story-index.bin`，"
               "缺失、损坏或存在 `/.esp-story-index.dirty` 时才重扫。改完卡请重启设备，"
               "或在设置里手动刷新索引。")
    out.append("- 标题就是文件名去掉扩展名，固件不做清洗：`睡前故事` 的 `01-`…`154-` 序号前缀会原样显示。")
    out.append("- `/video` 不在本索引里：视频列表由设备单独扫描，媒体索引只管音乐和故事。")
    out.append("- `docs/media-index.md` 记录的是索引机制本身；本文件是这张卡的内容清单。")
    out.append("- 重新整理文件后重跑：`python tools/build_media_index.py`。")
    out.append("")
    return "\n".join(out)


def build_music_markdown(music: list[Category], args: argparse.Namespace) -> str:
    """只输出 /music 的清单，给分享/打印用。"""
    tracks = [m for cat in music for m in cat.medias]
    now = datetime.now().strftime("%Y-%m-%d %H:%M")
    out: list[str] = ["# 音乐清单", ""]
    out.append(f"生成时间：{now} ｜ 来源：`{args.music}` ｜ "
               f"生成工具：`tools/build_media_index.py --music-only`")
    out.append("")
    out.append("卡片第一层目录就是设备上的音乐分类，分类内部递归收集 `.mp3`；"
               "标题取文件名（去扩展名），设备上显示的正是下表这一列。时长/码率由 ffprobe 读取。")
    out.append("")
    out.append("## 总览")
    out.append("")
    out.append(f"- 曲目：**{len(tracks)} 首**，**{len(music)} 个分类**")
    out.append(f"- 总时长：**{fmt_duration(sum(m.duration or 0 for m in tracks))}**")
    out.append(f"- 占用：**{sum(m.size for m in tracks) / 1024 / 1024:.1f} MB**")
    out.append(f"- 固件上限：曲目 {len(tracks)}/{LIMITS['tracks']}、分类 {len(music)}/{LIMITS['categories']}")
    out.append("")
    rows = [[cat.name, str(len(cat.medias)), fmt_duration(cat.total_duration),
             f"{cat.total_size / 1024 / 1024:.1f} MB"] for cat in music]
    rows.append(["**合计**", f"**{len(tracks)}**",
                 f"**{fmt_duration(sum(m.duration or 0 for m in tracks))}**",
                 f"**{sum(m.size for m in tracks) / 1024 / 1024:.1f} MB**"])
    out += table(rows, ["分类（= SD 卡目录）", "曲目数", "总时长", "占用"])
    out.append("")

    for position, cat in enumerate(music, 1):
        out.append(f"## {position}. {cat.name}")
        out.append("")
        out.append(f"{len(cat.medias)} 首，{fmt_duration(cat.total_duration)}，"
                   f"{cat.total_size / 1024 / 1024:.1f} MB")
        out.append("")
        rows = []
        for index, media in enumerate(cat.medias, 1):
            rows.append([str(index), media.title, fmt_duration(media.duration),
                         f"{media.size / 1024 / 1024:.1f} MB"])
        out += table(rows, ["#", "曲名（= 文件名）", "时长", "大小"])
        out.append("")

    out.append("## 说明")
    out.append("")
    out.append("- 设备按文件名排序显示（FAT 目录顺序），本表按名称自然排序；"
               "想调整播放顺序就改文件名前缀（如 `01-`）。")
    out.append("- 歌名里的歌手/专辑标记会原样显示，需要干净标题就得改文件名。")
    out.append("- 重新生成：`python tools/build_media_index.py --music-only`；"
               "带故事和自检的完整索引用 `python tools/build_media_index.py`。")
    out.append("")
    return "\n".join(out)


def write_csv(path: Path, music: list[Category], stories: list[Category]) -> None:
    import csv

    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["类型", "分类", "标题", "时长(秒)", "时长", "大小(MB)",
                         "码率(kbps)", "采样率(Hz)", "声道", "相对路径"])
        for label, cats in (("音乐", music), ("故事", stories)):
            for cat in cats:
                for media in cat.medias:
                    writer.writerow([
                        label, cat.name, media.title,
                        f"{media.duration:.2f}" if media.duration else "",
                        fmt_duration(media.duration),
                        f"{media.size / 1024 / 1024:.2f}",
                        f"{media.bitrate // 1000}" if media.bitrate else "",
                        media.sample_rate or "",
                        media.channels or "",
                        media.rel,
                    ])


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 SD 卡媒体索引")
    parser.add_argument("--music", default=r"D:\music", help="/music 目录")
    parser.add_argument("--story", default=r"D:\story", help="/story 目录")
    parser.add_argument("--out-dir", default=r"D:\\", help="输出目录")
    parser.add_argument("--jobs", type=int, default=8, help="ffprobe 并发数")
    parser.add_argument("--music-only", action="store_true",
                        help="只扫描 /music，输出 音乐清单.md")
    args = parser.parse_args()
    LIMITS.update(load_limits())

    music_root = Path(args.music)
    story_root = Path(args.story)
    roots = [music_root] if args.music_only else [music_root, story_root]
    for root in roots:
        if not root.is_dir():
            print(f"目录不存在：{root}", file=sys.stderr)
            return 1

    started = time.time()
    music = scan_music(music_root)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.music_only:
        medias = [m for cat in music for m in cat.medias]
        print(f"扫描到 {len(music)} 个音乐分类、{len(medias)} 首，开始读取时长…")
        probe_all(medias, args.jobs)
        md_path = out_dir / "音乐清单.md"
        md_path.write_text(build_music_markdown(music, args), encoding="utf-8", newline="\n")
        failed = [m for m in medias if m.error]
        print(f"完成：{md_path}")
        print(f"耗时 {time.time() - started:.1f}s；读取失败 {len(failed)} 个")
        for media in failed[:10]:
            print(f"  ! {media.rel}: {media.error}")
        return 0

    stories, illustrated = scan_story(story_root)
    medias = [m for c in music for m in c.medias] + [m for c in stories for m in c.medias]
    print(f"扫描到 {len(music)} 个音乐分类 / {len(stories)} 个故事分类，共 {len(medias)} 个音频，开始读取时长…")
    probe_all(medias, args.jobs)

    md_path = out_dir / "媒体索引.md"
    csv_path = out_dir / "媒体索引.csv"
    md_path.write_text(build_markdown(music, stories, illustrated, args),
                       encoding="utf-8", newline="\n")
    write_csv(csv_path, music, stories)

    failed = [m for m in medias if m.error]
    print(f"完成：{md_path}")
    print(f"完成：{csv_path}")
    print(f"耗时 {time.time() - started:.1f}s；读取失败 {len(failed)} 个")
    for media in failed[:10]:
        print(f"  ! {media.rel}: {media.error}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
