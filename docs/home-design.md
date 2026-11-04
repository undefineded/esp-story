# 故事小镇主页

IDF 首页由 `components/app/src/pages_home.cpp` 实现，四个入口依次为音乐、故事、视频和设置。屏幕左侧显示主题与探索按钮，右侧为 128×128 插画，底部为车票导航。

音乐、故事和设置图来自 `docs/icons/home-scenes.png`，经 `python3 tools/build_home_scenes.py` 生成 `src/home_scenes.h`。视频图来自 `docs/icons/video-theater.png`，使用 `python3 tools/build_video_theater.py` 生成 `src/home_video_scene.h`。两者需要 Pillow；生成的 byte-swapped RGB565 数组驻留 Flash，运行时不解码 PNG。

视频背景烘焙为 `#f8dfd4`，修改时应与 `pages_home.cpp` 中视频主题的 `sky` 保持一致。车票小图标由页面代码绘制。`tools/build_video_scene.mjs` 和 `docs/icons/video-scene.png` 是旧视频插画，不是当前资源；`home-preview.html/png` 是早期方案，仅作参考。

生成资源后运行 `idf.py build`，并在实机确认颜色、边缘和旋钮操作。
