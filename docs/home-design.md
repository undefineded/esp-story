# 故事小镇主页

主页沿用现有四个入口及顺序：音乐、故事、视频、设置。旋钮或 NEXT/LAST 切换，OK / 编码器短按进入，业务逻辑保持在 `main.cpp`。

画面为 320×240：顶部品牌及状态栏，左侧主题名称和探索按钮，右侧 128×128 插画，底部彩色车票导航。四个主题分别为音乐舞台、故事森林、视频剧场、工具小屋。卡片检测和童锁标识保留，音量更新不重画插画。

## 资源生成

`docs/icons/home-scenes.png` 是四宫格透明插画原图，依次为小熊舞台、树屋、小鸟、猫头鹰工坊；其中第 3 格（童谣小鸟）不再使用，主页视频格改由 `docs/icons/video-scene.png`（电视机 + 播放键）提供。视频插画用代码绘制，方便以后改配色/构图：

```sh
node tools/build_video_scene.mjs
```

脚本依赖 Node 和 ffmpeg（不需要 Pillow）：Node 逐像素画出 627×627 原图并交给 ffmpeg 编码 PNG，再缩到 128×128、转成 swap565，写入 `src/home_video_scene.h`。另外三格继续使用 `src/home_scenes.h`（由 `home-scenes.png` 烘焙，共 131072 字节），以只读数组存放在 Flash；运行时不依赖 TF 卡、PNG 解码或图像缩放。改动视频插画底色时须同步 `src/ui.cpp` 的 `HOME_THEMES[2].sky`，避免插画出现方形色差。

需要重做另外三格时可以运行 `python3 tools/build_home_scenes.py`（依赖 Pillow）：它会从 `home-scenes.png` 重新生成四格，其中第 3 格仍是旧的小鸟图，但主页不再使用（视频格固定取 `home_video_scene.h`）。

车票上的小图标（音符/书/放映机/齿轮）由 `src/ui.cpp` 的 `homeTile()` 直接绘制，不依赖图片资源。

文字使用固件现有的 efont 16/14/12 像素字体。HTML 设计稿使用 VLW 字库，因此实际点阵字形与设计稿略有不同。切换主题时重绘 PSRAM 合成画布，由现有 `Display::present` 差分提交；没有选择或音量变化时不重绘。未新增全屏缓冲区。

> `docs/home-preview.png` / `home-preview.html` 是早期“四宫格大图标”设计稿，与当前车票式主页不一致，仅作历史参考；以 `src/ui.cpp` 的 `drawHome()` 为准。

## 编译验证

本次使用临时配置指定 `platform = espressif32@6.12.0`（Arduino 2.0.17）完成编译和链接：静态 RAM 155596 字节，Flash 2461473 字节。仓库原配置没有固定平台版本，未在此次主页修改中调整；全新安装可能选中 Arduino 3.x，而现有背光等驱动仍使用 2.x 接口。尚未进行实体屏幕和旋钮测试。
