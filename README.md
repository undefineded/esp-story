# 小小故事盒

基于 ESP32-S3 的儿童音乐播放器和故事机固件。设备使用 320×240 ST7789 屏幕、TF 卡、MAX98357A I2S 功放、EC11 旋钮和按键，支持音乐分类播放、故事插画跟随音频进度切换、亮度/旋转/EQ 设置。

## 功能

- 音乐模式：按分类浏览 MP3，支持播放、暂停、上一首、下一首和音量调节。
- 故事模式：读取 `/story/<故事名>/` 中的 MP3、插画和 `timeline.lrc`，按播放进度切换画面。
- 设置：亮度、横屏方向、EQ、自动息屏（关闭/1/5/10/30 分钟），以及“刷新列表”。自动息屏仅关闭背光，音频持续播放；按任意键、旋转旋钮或霍尔状态变化即可唤醒。
- efont 中文点阵字体：标题和正文 16px，故事标题 14px，辅助信息 12px，按原生尺寸绘制。
- PSRAM 画布和局部提交：只把变化区域发送到屏幕，减少刷新闪烁和 SPI 传输。
- SD 媒体索引：首次启动或手动刷新时扫描文件，之后启动和页面切换直接读取索引。

## 硬件

| 部件 | 接口/引脚 |
| --- | --- |
| ST7789 TFT | SCK 5，MOSI 4，DC 6，RST 7，CS 8，背光 9 |
| MicroSD | CLK 12，CMD 11，DAT0 13，1-bit SDMMC |
| MAX98357A | BCLK 16，LRC 17，DOUT 18 |
| OK / NEXT / LAST | 47 / 48 / 39 |
| EC11 编码器 | A 14，B 15，SET 21 |
| 霍尔开关 | 40 |
| I2C FRAM | SDA 1，SCL 2 |

完整引脚和设备限制见 [src/config.h](src/config.h)。当前限制为 300 首音乐、32 个音乐分类、32 个故事，每个故事最多 256 幅插画。

## 构建和烧录

需要 PlatformIO、Python 3 和可用的 PlatformIO 网络访问。

```sh
cd esp-story
python3 -m pip install --target /tmp/codex-platformio platformio
PYTHONPATH=/tmp/codex-platformio \
PLATFORMIO_CORE_DIR=/tmp/codex-platformio-core \
python3 -m platformio run
```

烧录和串口监视：

```sh
PYTHONPATH=/tmp/codex-platformio \
PLATFORMIO_CORE_DIR=/tmp/codex-platformio-core \
python3 -m platformio run --target upload

PYTHONPATH=/tmp/codex-platformio \
PLATFORMIO_CORE_DIR=/tmp/codex-platformio-core \
python3 -m platformio device monitor
```

固件目标为 `esp32-s3-devkitm-1`，串口速度为 115200，烧录速度为 921600。启动日志中应看到 `efontCN` 和 `PSRAM canvas: OK`。实际烧录需要连接 ESP32-S3 硬件；云端环境只能验证编译。

## TF 卡目录

音乐文件放在 `/music` 下。根目录中的 MP3 属于“全部歌曲”，一级子目录成为分类，子目录内的 MP3 会递归收录：

```text
/music/
  01.mp3
  1-2岁/
    小兔子.mp3
  3-5岁/
    森林.mp3
```

故事文件夹放在 `/story` 下，推荐使用故事构建脚本生成设备目录：

```text
/story/月亮邮递员/
  月亮邮递员.mp3
  01.jpg
  02.jpg
  timeline.lrc
```

`timeline.lrc` 的每行格式为：

```text
[00:00.00]01.jpg | 故事开始
[00:08.50]02.jpg | 月亮升起来了
```

可以使用 `tools/build_story.mjs` 将素材转换成 320×240 JPG、单声道 MP3 和设备时间轴。该脚本需要 Node.js 和 ffmpeg。

## 媒体索引

设备会在 SD 卡根目录生成 `/.esp-story-index.bin`。首次启动没有有效索引时扫描 `/music` 和 `/story`；以后直接加载索引，不再重复枚举目录。

增删、重命名或更换媒体后，在设置中选择“刷新列表”，单击 OK 重新扫描并保存索引。刷新期间会停止播放，请勿拔出 TF 卡。索引包含版本和校验值，并使用临时文件及备份文件更新。详细行为见 [docs/media-index.md](docs/media-index.md)。

## 操作

| 操作 | 音乐列表/歌曲列表 | 播放页 | 故事列表/故事播放 |
| --- | --- | --- | --- |
| 旋转编码器 | 移动选择/调节设置 | 调节音量 | 移动选择/调节音量 |
| NEXT / LAST 短按 | 移动选择 | 下一首/上一首 | 移动选择 |
| NEXT / LAST 长按 | — | 进入歌曲列表/返回分类列表 | — |
| OK 单击 | 播放/进入 | 播放或暂停 | 播放/进入、暂停 |
| OK 双击 | 进入设置 | 进入设置 | 进入设置 |
| OK 长按 | — | 停止并返回列表 | 返回故事列表 |
| SET 短按 | 播放选中项 | 播放或暂停 | 播放选中故事 |
| SET 长按 | 切换音乐/故事模式 | 切换音乐/故事模式 | 切换音乐/故事模式 |

在设置页使用 NEXT/LAST 选择项目，旋钮或 OK 修改；双击 OK 保存并返回。

## 项目结构

```text
src/display.*       ST7789、PSRAM 画布和局部提交
src/ui.*            页面绘制和 UI 状态缓存
src/fonts.*         efont 点阵字体选择
src/player.*        ESP32-audioI2S 和音频 FreeRTOS 任务
src/storage.*       音乐列表和分类
src/story.*         故事目录、时间轴和插画切换
src/catalog.*       SD 媒体索引加载、扫描和保存
tools/              故事、时间轴和字体预览工具
test/               主机端索引及渲染逻辑测试
docs/               刷新、索引和字体对比说明
```

## 验证

固件构建：

```sh
PYTHONPATH=/tmp/codex-platformio \
PLATFORMIO_CORE_DIR=/tmp/codex-platformio-core \
python3 -m platformio run
```

主机端逻辑测试：

```sh
g++ -std=c++11 -Wall -Wextra -pedantic -fsanitize=address,undefined \
  test/render_math_test.cpp -o /tmp/esp-story-render-test
/tmp/esp-story-render-test

g++ -std=c++11 -Wall -Wextra -pedantic -fsanitize=address,undefined \
  -Itest/catalog_host -Isrc src/catalog.cpp test/catalog_host/test_catalog.cpp \
  -o /tmp/esp-story-catalog-test
/tmp/esp-story-catalog-test
```

显示刷新方案和硬件验收项目见 [docs/display-refresh.md](docs/display-refresh.md)；字体方案对比见 [docs/font-comparison.html](docs/font-comparison.html)。

## 当前限制

- JPEG 解码和媒体刷新仍在主任务中执行，故事切图的实际流畅度需要硬件验证。
- PSRAM 画布减少了中间态和无效传输，但不保证整屏切换完全无撕裂。
- 媒体索引不会自动发现新增文件，修改 SD 卡内容后必须手动刷新。
- 没有连接实体硬件时无法验证屏幕颜色、按键方向、音频爆音和真实帧率。
