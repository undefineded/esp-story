/*
 * 硬件配置：引脚定义与全局常量
 * 全部来自 Netlist_Schematic1_2026-10-08.enet
 */
#pragma once
#include <Arduino.h>

/* --- ST7789 TFT (SPI, 240x320) --- */
#define TFT_SCK    5    // SCL_SCREEN -> IO5
#define TFT_MOSI   4    // SDA_SCREEN -> IO4
#define TFT_DC     6    // D/C        -> IO6
#define TFT_RST    7    // RESET      -> IO7
#define TFT_CS     8    // CS         -> IO8
#define TFT_BL     9    // BL 经 Q1(SS8050) 驱动, 高电平点亮 -> IO9 (PWM 调光)

/* --- MicroSD 卡座 (1-bit SDMMC) --- */
#define SD_CLK_PIN  12   // SD_CLK      -> IO12
#define SD_CMD_PIN  11   // SD_CMD      -> IO11
#define SD_D0_PIN   13   // SD_DAT0     -> IO13
#define SD_CD_PIN   10   // CD/DAT3     -> IO10 (卡检测, 低有效)

/* --- MAX98357A I2S 功放 --- */
#define I2S_BCLK   16   // MAX_BCLK  -> IO16
#define I2S_LRC    17   // MAX_LRCLK -> IO17
#define I2S_DOUT   18   // MAX_DIN   -> IO18

/* --- 按键 (按下拉低) --- */
#define BTN_OK     47   // OK   -> IO47
#define BTN_NEXT   48   // NEXT -> IO48
#define BTN_LAST   39   // LAST -> IO39

/* --- EC11 编码器 --- */
#define ENC_A      14   // ADD   -> IO14
#define ENC_B      15   // MINUS -> IO15
#define ENC_SW     21   // SET   -> IO21 (按下)

/* --- 霍尔开关 U9 (HAL251SO, 全极型推挽输出) -> IO40 --- */
#define HALL_PIN   40   // HALOUT -> IO40 (经 R29 10R)

/* --- I2C FRAM U2 (MB85RC16PNF, 16Kbit = 2KB, WP 接地可写) --- */
#define I2C_SDA    1    // SDA -> IO1
#define I2C_SCL    2    // SCL -> IO2

/* --- UI / 播放常量 --- */
#define SCR_W        320          // 横屏宽度
#define SCR_H        240          // 横屏高度
#define MUSIC_DIR    "/music"     // TF 卡音乐目录 (子文件夹 = 分类, 如 /music/1-2岁/)
#define STORY_DIR    "/story"     // TF 卡故事目录 (/story/<故事名>/)
#define MAX_TRACKS   300          // 最大曲目数
#define MAX_CATEGORIES 32         // 最大音乐分类数 (不含置顶的"全部歌曲")
#define MAX_STORIES  32           // 最大故事数
#define MAX_PANELS   256          // 每个故事最大插画数 (每个 Panel 32B, 256 -> 8KB)
#define VOL_MAX      21           // 音量档位 0..21
#define BRIGHT_MIN   10           // 最低亮度 (防止调黑后无法操作)
#define BRIGHT_MAX   100
#define ENC_LONG_MS  800          // 编码器 SET 长按判定阈值 (切换模式)
#define OK_LONG_MS   800          // OK 键长按判定阈值 (故事播放页返回列表)
#define NAV_LONG_MS  800          // NEXT/LAST 键长按判定阈值 (播放页进入分类列表)

/* --- 无线网络 (设置页可开关) ---
 * AP 默认 SSID: StoryBox-XXXX (XXXX 为蓝牙 MAC 后两位), 密码见 WIFI_AP_PASSWORD。
 * WiFi STA 默认没有账号: 把下面两行改成家里的 SSID / 密码即可在设置里打开 WiFi。
 * 也可以不修改这里, 在 platformio.ini 的 build_flags 里定义:
 *   -DWIFI_STA_SSID=\"MyWiFi\" -DWIFI_STA_PASS=\"12345678\"
 */
#ifndef WIFI_STA_SSID
#define WIFI_STA_SSID   ""
#endif
#ifndef WIFI_STA_PASS
#define WIFI_STA_PASS   ""
#endif
#define WIFI_AP_PASSWORD "storybox123"