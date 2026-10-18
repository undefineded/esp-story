/*
 *  ESP32-S3 音乐播放器 / 故事机
 *
 *  硬件（全部来自 Netlist_Schematic1_2026-10-08.enet）:
 *    MCU     : ESP32-S3-WROOM-1U-N16R8
 *    功放    : MAX98357A  (I2S, BCLK/LRCLK/DIN)
 *    TF 卡   : 1-bit SDMMC (CLK/CMD/D0)
 *    屏幕    : ST7789 240x320 SPI (LovyanGFX, BL 经 Q1 支持 PWM 调光)
 *    霍尔    : U9 HAL251SO -> IO40 (调试显示)
 *    按键    : OK / NEXT / LAST + EC11 编码器
 *
 *  组件:
 *    Display  - LovyanGFX ST7789 驱动 + 背光 PWM
 *    Storage  - TF 卡挂载与 /music 扫描
 *    Player   - ESP32-audioI2S 封装 + 人声 EQ 预设
 *    Story    - /story 故事扫描 + 时间轴插画同步
 *    Input    - 按键 / 编码器 (SET 支持长按)
 *    Hall     - 霍尔开关状态
 *    Fram     - I2C 铁电存储器 (U2 MB85RC16, 配置持久化)
 *    Settings - 亮度/旋转/EQ 配置 (掉电保存到 FRAM)
 *    Ui       - 自绘主界面 + 设置界面 + 故事模式界面
 *
 *  两种模式 (长按编码器 SET 切换):
 *    音乐模式 - /music (一级子文件夹为分类, 置顶"全部歌曲"递归收录全部 mp3)
 *    故事模式 - /story/<故事名>/ (mp3 + 插画 + timeline.lrc)
 *
 *  音乐分类列表操作 (开机默认界面):
 *    编码器/NEXT/LAST: 选择分类        OK 单击: 播放该分类
 *    SET 短按 : 播放当前分类           SET 长按: 进入故事模式
 *  音乐播放操作:
 *    OK 单击  : 播放 / 暂停           OK 双击 : 进入设置
 *    NEXT/LAST: 下一曲 / 上一曲
 *    NEXT 长按: 当前歌曲列表          LAST 长按 / OK 长按: 分类列表
 *    编码器   : 音量, 短按=播放/暂停
 *    SET 长按 : 进入故事模式
 *  当前歌曲列表操作:
 *    编码器/NEXT/LAST: 选择曲目       OK 单击 / SET 短按: 播放选中曲目
 *    长按     : 返回播放界面          SET 长按: 进入故事模式
 *  设置界面操作:
 *    编码器   : 调整当前项             NEXT/LAST: 上下选择
 *    OK 单击  : 递增当前项             OK 双击 : 保存并返回
 *  故事列表操作:
 *    编码器/NEXT/LAST: 选择故事        OK 单击 / SET 短按: 播放
 *    SET 长按 : 回音乐模式
 *  故事播放操作:
 *    OK 单击 / SET 短按: 暂停/继续     编码器  : 音量
 *    OK 双击  : 进入设置               OK 长按 : 停止并返回列表
 *    SET 长按 : 回音乐模式
 */

#include <Arduino.h>
#include "config.h"
#include "display.h"
#include "storage.h"
#include "player.h"
#include "story.h"
#include "input.h"
#include "hall.h"
#include "fram.h"
#include "settings.h"
#include "fonts.h"
#include "ui.h"

/* ---------------- 界面状态机 ---------------- */

enum class Screen : uint8_t { MusicList, TrackList, Main, Settings, StoryList, StoryPlay, StoryError };

static Screen s_screen   = Screen::MusicList;
static Screen s_settingsBack = Screen::Main;   // 设置界面来源屏 (保存后返回该屏)
static int    s_sel      = 0;      // 设置界面当前选中项 0..2
static int    s_musicSel = 0;      // 音乐分类列表当前选中项
static int    s_trackSel = 0;      // 当前歌曲列表选中项
static int    s_trackPlaying = 0;  // 当前歌曲列表中"正在播放"的曲目下标
static int    s_storySel = 0;      // 故事列表当前选中项
static bool   s_storyRedraw = false;  // 从设置返回故事页: 置位以强制整体重绘

static const int SETTINGS_ITEMS = 3;   // 亮度 / 旋转 / EQ

static void applySettings() {
  SettingsData& s = Settings::data();
  Display::setBrightness(s.brightness);
  Display::setRotation(s.rotation);
  Player::setEq(s.eq);
}

static void enterSettings() {
  s_settingsBack = s_screen;        // 记录来源屏, 保存后返回该屏
  s_screen = Screen::Settings;
  s_sel    = 0;
  Ui::drawSettings(Settings::data(), s_sel);
}

static void exitSettings() {
  Settings::save();                 // 保存到 NVS
  if (s_settingsBack == Screen::StoryPlay) {
    s_screen      = Screen::StoryPlay;   // 返回故事播放页
    s_storyRedraw = true;                // 插画+信息条由下一帧整体重绘
  } else {
    s_screen = Screen::Main;
    Ui::drawStatic();
    Ui::update(Player::status(), Hall::detected());
  }
}

/* 调整设置项: item 0=亮度, 1=旋转, 2=EQ; delta 为编码器步数/单击 +1 */
static void adjustSetting(int item, int delta) {
  SettingsData& s = Settings::data();

  if (item == 0) {
    int v = (int)s.brightness + delta * 5;
    s.brightness = (uint8_t)constrain(v, BRIGHT_MIN, BRIGHT_MAX);
    Display::setBrightness(s.brightness);
    Ui::refreshSettings(s, item);
  } else if (item == 1) {
    s.rotation = (s.rotation == 1) ? 3 : 1;
    Display::setRotation(s.rotation);
    Ui::drawSettings(s, item);      // 旋转改变了显示方向, 需整屏重绘
  } else {
    int e = ((int)s.eq + (delta >= 0 ? 1 : -1) + (int)EqPreset::COUNT) % (int)EqPreset::COUNT;
    s.eq = (EqPreset)e;
    Player::setEq(s.eq);
    Ui::refreshSettings(s, item);
  }
}

/* ---------------- 音乐分类 ---------------- */

/* 绘制音乐分类列表 (置顶"全部歌曲" + /music 一级子文件夹) */
static void showMusicList() {
  int n = Storage::catCount();
  if (s_musicSel >= n) s_musicSel = 0;
  s_screen = Screen::MusicList;
  Ui::drawMusicList(n, s_musicSel);
}

/* 播放当前分类中的第 i 首, 并回到播放界面 */
static void playMusicIndex(int i) {
  s_trackSel = i;
  Player::playIndex(i, true);
  s_screen = Screen::Main;
  Ui::drawStatic();
  Ui::update(Player::status(), Hall::detected());
}

/* 绘制当前分类的歌曲列表 (正在播放的曲目高亮), 无曲目则回分类列表 */
static void showTrackList() {
  int n = Storage::count();
  if (n == 0) { showMusicList(); return; }
  PlayerStatus st = Player::status();
  s_trackPlaying = (st.index >= 0 && st.index < n) ? st.index : 0;
  s_trackSel     = s_trackPlaying;
  s_screen = Screen::TrackList;
  Ui::drawTrackList(n, s_trackSel, s_trackPlaying);
}

/* 选定分类 (0=全部歌曲) 并开始播放该分类第一首 */
static void startMusicCategory(int ci) {
  if (!Storage::selectCategory(ci)) {          // 该分类无曲目: 停留列表
    Serial.printf("[MUS] category %d has no track\n", ci);
    return;
  }
  s_musicSel = ci;
  playMusicIndex(0);
}

/* ---------------- 故事模式 ---------------- */

static int s_storyPanel = -1;            // 已绘制的插画下标

/* 控制条刷新: Ui 内部按变化区域增量重绘(含故事名跑马灯), 每帧调用开销极小 */
static void refreshStoryBar() {
  PlayerStatus st = Player::status();
  Ui::drawStoryBar(Story::panelIndex(), Story::panelCount(),
                   st.curSec, st.durSec, st.volume, st.paused);
}

/* 绘制故事列表; 一个故事都没有时进入提示页 */
static void showStoryList() {
  int n = Story::count();
  if (n == 0) {
    s_screen = Screen::StoryError;
    Ui::boot("NO STORY FOUND", "Copy story folders to /story");
    return;
  }
  if (s_storySel >= n) s_storySel = 0;
  s_screen = Screen::StoryList;
  Ui::drawStoryList(n, s_storySel);
}

static void enterStoryMode() {
  Story::stop();                    // 切换模式: 停掉音乐/上一个故事
  Story::begin();                   // 重新扫描 /story
  s_storySel = 0;
  showStoryList();
}

static void exitStoryMode() {
  Story::stop();
  showMusicList();                  // 回音乐分类列表
}

static void startStory(int i) {
  /* 按下确定立刻进入加载画面; 真实的目录/时间轴读取就在加载页显示期间完成
     (此时列表页没有音频播放, 阻塞是安全的) */
  String name = Story::title(i);
  Ui::setStoryTitle(name.c_str());          // 控制条显示的故事名
  Ui::drawStoryLoadingBegin(name.c_str());
  if (!Story::open(i)) {
    s_screen = Screen::StoryError;
    Ui::boot("STORY CANNOT PLAY", "Missing mp3 or timeline.lrc");
    return;
  }
  for (int f = 1; f < 14; f++) {            // 圆环继续转约 1.6 圈
    Ui::drawStoryLoadingSpin(f);
    delay(55);
  }

  Story::start();
  s_storyPanel = Story::panelIndex();
  s_screen     = Screen::StoryPlay;
  Ui::drawStoryFrame(Story::panelPath());
  Ui::invalidateStoryBar();          // 插画覆盖了控制条区, 需整体重铺
  refreshStoryBar();
}

/* ---------------- setup / loop ---------------- */

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32-S3 Music Player ===");
  Serial.printf("[SYS] PSRAM %s, size: %u bytes\n",
                psramFound() ? "OK" : "NOT FOUND", ESP.getPsramSize());

  Display::begin();

  Fram::begin();                    // I2C FRAM (设置持久化)
  Serial.printf("[SYS] FRAM %s\n", Fram::ok() ? "OK" : "NOT FOUND");
  Settings::load();                 // 读取掉电保存的配置
  applySettings();

  Fonts::begin();                   // 加载圆体抗锯齿字库 (失败自动回退点阵)
  Ui::begin();
  Ui::boot("MUSIC PLAYER", "Mounting TF card ...");

  Input::begin();
  Hall::begin();

  if (!Storage::begin()) {
    Ui::boot("SD CARD ERROR", "Insert TF card then reset");
  } else if (Storage::totalCount() == 0) {
    Ui::boot("NO MUSIC FOUND", "Copy MP3 folders to /music");
  } else {
    Ui::boot("MUSIC PLAYER", "Ready - press OK to select");
  }
  delay(600);

  Player::begin();
  Player::setEq(Settings::data().eq);

  if (Storage::totalCount() > 0) {
    showMusicList();                // 有音乐: 开机进入分类列表
  } else {
    s_screen = Screen::Main;
    Ui::drawStatic();
    Ui::update(Player::status(), Hall::detected());
  }
}

void loop() {
  /* 解码在独立 FreeRTOS 音频任务里持续运行, 这里只处理界面与输入 */

  switch (s_screen) {

  /* ---------------- 音乐分类列表 ---------------- */
  case Screen::MusicList: {
    int8_t enc = Input::encEvent();
    if (enc == 2) { enterStoryMode(); return; }                  // SET 长按: 进入故事模式
    if (enc == 1) { startMusicCategory(s_musicSel); return; }    // SET 短按: 播放选中分类

    int move = Input::encoderSteps();
    if (Input::nextEvent() == 1) move += 1;
    if (Input::prevEvent() == 1) move -= 1;

    int n = Storage::catCount();
    if (move != 0 && n > 0) {
      s_musicSel = (int)((s_musicSel + move % n + n) % n);
      Ui::updateMusicList(n, s_musicSel);
      return;
    }

    if (Input::okEvent() == 1) { startMusicCategory(s_musicSel); return; }
    break;
  }

  /* ---------------- 当前歌曲列表 ---------------- */
  case Screen::TrackList: {
    int n = Storage::count();

    int8_t enc = Input::encEvent();
    if (enc == 2) { enterStoryMode(); return; }                 // SET 长按: 进入故事模式
    if (enc == 1) { playMusicIndex(s_trackSel); return; }       // SET 短按: 播放选中曲目

    int8_t nx = Input::nextEvent();
    int8_t pv = Input::prevEvent();
    if (nx == 2 || pv == 2) {                                   // 长按: 返回播放界面(保持播放)
      s_screen = Screen::Main;
      Ui::drawStatic();
      Ui::update(Player::status(), Hall::detected());
      return;
    }

    int move = Input::encoderSteps() + (nx == 1 ? 1 : 0) + (pv == 1 ? -1 : 0);
    if (move != 0 && n > 0) {
      s_trackSel = (int)((s_trackSel + move % n + n) % n);
      Ui::updateTrackList(n, s_trackSel, s_trackPlaying);
      return;
    }

    switch (Input::okEvent()) {
      case 1: playMusicIndex(s_trackSel); return;               // 单击: 播放选中曲目
      case 3:                                                   // 长按: 返回播放界面(保持播放)
        s_screen = Screen::Main;
        Ui::drawStatic();
        Ui::update(Player::status(), Hall::detected());
        return;
      default: break;
    }

    /* 自动续播换曲时同步"播放中"高亮 */
    static uint32_t lastSync = 0;
    if (millis() - lastSync > 500) {
      lastSync = millis();
      int cur = Player::status().index;
      if (cur >= 0 && cur < n && cur != s_trackPlaying) {
        s_trackPlaying = cur;
        Ui::updateTrackList(n, s_trackSel, s_trackPlaying);
      }
    }
    break;
  }

  /* ---------------- 音乐播放界面 ---------------- */
  case Screen::Main: {
    int8_t enc = Input::encEvent();
    if (enc == 2) { enterStoryMode(); return; }   // SET 长按: 进入故事模式
    if (enc == 1) Player::toggle();               // SET 短按: 播放/暂停

    int8_t nx = Input::nextEvent();
    int8_t pv = Input::prevEvent();
    if (nx == 1)      Player::next();                       // 短按: 下一曲
    else if (nx == 2) { showTrackList(); return; }          // 长按: 当前歌曲列表
    if (pv == 1)      Player::prev();                       // 短按: 上一曲
    else if (pv == 2) { Player::stop(); showMusicList(); return; }   // 长按: 分类列表

    switch (Input::okEvent()) {
      case 1: Player::toggle(); break;    // 单击: 播放/暂停
      case 2: enterSettings();  return;   // 双击: 进入设置
      case 3: Player::stop(); showMusicList(); return;   // 长按: 停止并返回分类列表
      default: break;
    }

    int steps = Input::encoderSteps();
    if (steps) Player::adjustVolume(steps);

    static uint32_t lastUi = 0;
    if (millis() - lastUi > 250) {
      lastUi = millis();
      Ui::update(Player::status(), Hall::detected());
    }
    break;
  }

  /* ---------------- 设置界面 ---------------- */
  case Screen::Settings: {
    if (Input::nextEvent() == 1) { s_sel = (s_sel + 1) % SETTINGS_ITEMS; Ui::refreshSettings(Settings::data(), s_sel); }
    if (Input::prevEvent() == 1) { s_sel = (s_sel - 1 + SETTINGS_ITEMS) % SETTINGS_ITEMS; Ui::refreshSettings(Settings::data(), s_sel); }

    switch (Input::okEvent()) {
      case 1: adjustSetting(s_sel, +1); break;   // 单击: 递增
      case 2: exitSettings();           return;  // 双击: 保存返回
      default: break;
    }

    int steps = Input::encoderSteps();
    if (steps) adjustSetting(s_sel, steps);
    break;
  }

  /* ---------------- 故事列表 ---------------- */
  case Screen::StoryList: {
    int8_t enc = Input::encEvent();
    if (enc == 2) { exitStoryMode(); return; }    // SET 长按: 回音乐模式
    if (enc == 1) { startStory(s_storySel); return; }

    int move = Input::encoderSteps();
    if (Input::nextEvent() == 1) move += 1;
    if (Input::prevEvent() == 1) move -= 1;

    int n = Story::count();
    if (move != 0 && n > 0) {
      s_storySel = (int)((s_storySel + move % n + n) % n);
      Ui::updateStoryList(n, s_storySel);
      return;
    }

    if (Input::okEvent() == 1) { startStory(s_storySel); return; }
    break;
  }

  /* ---------------- 故事播放 ---------------- */
  case Screen::StoryPlay: {
    Story::loop();

    if (Story::takeFinished()) {                  // 播放完毕 -> 回列表
      Story::stop();
      showStoryList();
      return;
    }

    if (s_storyRedraw || Story::panelIndex() != s_storyPanel) {   // 换插画 / 从设置返回
      s_storyRedraw = false;
      s_storyPanel  = Story::panelIndex();
      Ui::drawStoryFrame(Story::panelPath());
      Ui::invalidateStoryBar();                      // 插画覆盖控制条区, 需整体重铺
    }

    int8_t enc = Input::encEvent();
    if (enc == 2) { exitStoryMode(); return; }    // SET 长按: 回音乐模式
    if (enc == 1) Player::toggle();               // SET 短按: 暂停/继续

    switch (Input::okEvent()) {
      case 1: Player::toggle(); break;              // 单击: 暂停/继续
      case 2: enterSettings(); return;              // 双击: 进入设置
      case 3: Story::stop(); showStoryList();  return;   // 长按: 停止并返回列表
      default: break;
    }

    int steps = Input::encoderSteps();
    if (steps) Player::adjustVolume(steps);

    refreshStoryBar();                              // 每帧刷新, 内部只重绘变化区域
    break;
  }

  /* ---------------- 故事提示页 ---------------- */
  case Screen::StoryError: {
    int8_t enc = Input::encEvent();
    if (enc == 2) { exitStoryMode(); return; }    // SET 长按: 回音乐模式

    bool back = (enc == 1) || Input::nextEvent() == 1 || Input::prevEvent() == 1;
    int8_t ok = Input::okEvent();
    if (ok == 1 || ok == 2) back = true;

    if (back) {
      Story::begin();
      s_storySel = 0;
      showStoryList();
    }
    break;
  }

  }
}
