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
 *    Video    - /video MJPEG AVI 视频播放 (索引 + 时间轴取帧)
 *    Input    - 按键 / 编码器 (SET 支持长按)
 *    Hall     - 霍尔开关状态
 *    Fram     - I2C 铁电存储器 (U2 MB85RC16, 配置持久化)
 *    Settings - 亮度/旋转/EQ 配置 (掉电保存到 FRAM)
 *    Session  - 上次的分类/曲目/音量记忆 (掉电保存到 FRAM)
 *    Ui       - 自绘主页 + 播放/列表 + 设置 + 故事界面
 *
 *  主页 (开机默认界面, 四个功能块):
 *    音乐 - /music (一级子文件夹为分类, 置顶"全部歌曲"递归收录全部 mp3)
 *    故事 - /story/<故事名>/ (mp3 + 插画 + timeline.lrc)
 *    视频 - /video/*.avi (MJPEG) + 同名 mp3 音轨, 全屏播放
 *    设置 - 亮度/旋转/EQ/刷新列表/无线开关/绑定码
 *    编码器/NEXT/LAST: 移动焦点        OK 单击 / SET 短按: 进入
 *    主页不再用"OK 双击"进设置, 也不再靠"SET 长按"切换音乐/故事模式。
 *
 *  通用返回:
 *    任意内容页 SET 长按: 回到主页 (故事播放中会先停止故事; 音乐继续播放)
 *    设置页 SET 长按: 保存并回主页
 *
 *  音乐分类列表 (每页 4 行):
 *    编码器/NEXT/LAST: 选择分类        OK 单击 / SET 短按: 播放该分类
 *  音乐播放:
 *    OK 单击: 播放/暂停   NEXT/LAST: 下一曲/上一曲
 *    NEXT 长按: 当前歌曲列表   LAST 长按 / OK 长按: 分类列表
 *    编码器: 音量          SET 短按: 播放/暂停
 *  当前歌曲列表:
 *    编码器/NEXT/LAST: 选择曲目        OK 单击 / SET 短按: 播放选中曲目
 *    OK 长按: 收藏/取消收藏            NEXT/LAST 长按: 返回播放界面
 *  设置界面:
 *    编码器: 调整当前项   NEXT/LAST: 上下选择
 *    OK 单击: 递增当前项  OK 双击: 保存并返回
 *    SET 长按: 保存并回主页
 *  故事列表 (每页 4 行):
 *    编码器/NEXT/LAST: 选择故事        OK 单击 / SET 短按: 播放
 *  故事播放:
 *    OK 单击 / SET 短按: 暂停/继续     编码器: 音量
 *    OK 长按: 停止并返回列表           SET 长按: 停止并回主页
 */

#include <Arduino.h>
#include "filexfer.h"
#include "config.h"
#include "display.h"
#include "storage.h"
#include "player.h"
#include "story.h"
#include "video.h"
#include "input.h"
#include "hall.h"
#include "fram.h"
#include "settings.h"
#include "session.h"
#include "fonts.h"
#include "ui.h"
#include "parental.h"
#include "favorites.h"
#include "power.h"
#include "ble.h"
#include "network.h"
#include "nfc.h"
#include "hostlink.h"
#include "sysmon.h"
#ifdef STORY_AUDIO_PREVIEW
#include <SD_MMC.h>
#endif

/* ---------------- 界面状态机 ---------------- */

enum class Screen : uint8_t { Home, MusicList, TrackList, VideoList, Main, Settings,
                              StoryCategoryList, StoryList, StoryPlay, StoryAudio, StoryError,
                              VideoPlay, VideoError,
                              NoContent, Empty, Locked, Bind };

static Screen s_screen   = Screen::Home;
static Screen s_settingsBack = Screen::Home;   // 设置界面来源屏 (保存后返回该屏)
static Screen s_lockedBack   = Screen::Home;   // 锁定前所在屏 (解锁后返回)
static Screen s_bindBack     = Screen::Home;   // 绑定码页返回屏
static int    s_homeSel  = 0;      // 主页当前选中块: 0 音乐 / 1 故事 / 2 视频 / 3 设置
static int    s_sel      = 0;      // 设置界面当前选中项
static int    s_musicSel = 0;      // 音乐分类列表当前选中项
static int    s_trackSel = 0;      // 当前歌曲列表选中项
static int    s_trackPlaying = 0;  // 当前歌曲列表中"正在播放"的曲目下标
static int    s_storySel = 0;      // 当前故事目录选中项
static int    s_storyCatSel = 0;   // /story 一级分类选中项
static int    s_videoSel = 0;      // 视频列表选中项
static uint32_t s_videoStartAt = 0;  // 本次起播时刻 (片名显示 3 秒)
static uint32_t s_videoActAt   = 0;  // 最近一次操作时刻 (控制条显示 4 秒)
static String s_storyPath;         // 当前浏览的故事目录 (/story 或更深)
static int    s_storyPlayIndex = 0;  // 当前目录播放列表中的序号
static int    s_storyPlayCount = 1;  // 当前目录可播放条目数
static bool   s_storyRedraw = false;  // 从设置返回故事页: 置位以强制整体重绘
static bool   s_audioRedraw = false;  // 音频故事页: 置位以强制整页重绘
static bool   s_bleWasConnected = false;
static uint32_t s_bindUntil = 0;
static Parental::Lock s_lockShown = Parental::Lock::None;

static const int SETTINGS_ITEMS = 10;  // 亮度 / 旋转 / EQ / 息屏 / 休眠 / 刷新 / 蓝牙 / WiFi / AP / 绑定码
static void showHome();
static void goHome();
static void showMusicList();
static void showStoryCategoryList();
static void showStoryList();
static void showStoryListAt(const String& path);
static void startStory(int view);
static void playStoryActual(int actual);
static void showTrackList();
static void showVideoList();
static void playVideo(int i);
static void stopVideoToList();
static void showNoContent();
static void showEmpty(const char* title, const char* line1, const char* line2);
static void showBindCode(Screen back);
static void restoreBindBack();
static void applyParentalConfig();

static void applySettings() {
  SettingsData& s = Settings::data();
  Display::setBrightness(s.brightness);
  Display::setRotation(s.rotation);
  Player::setEq(s.eq);
}

/* ---------------- 主页 ---------------- */

/* 回到主页: 故事播放中先停掉 (主页没有故事控制条); 音乐保持播放, 只是回菜单 */
static void showHome() {
  s_screen = Screen::Home;
  Ui::drawHome(s_homeSel, Player::status().volume);
}

static void goHome() {
  if (Story::active()) Story::stop();
  if (Video::active()) Video::stop();
  showHome();
}

/* ---------------- 会话记忆 (FRAM) ---------------- */

static void applyContentFilters() {
  Storage::setCategoryFilter(Parental::catMask(), Parental::allowlistEnabled());
  Story::setStoryFilter(Parental::storyMask(), Parental::allowlistEnabled());
}

static void setVolumeLimited(int v) {
  int cv = Parental::clampVolume(v);
  Player::setVolume(cv);
  Session::setVolume((uint8_t)cv);
}

static bool categoryVisible(int actual) {
  for (int i = 0; i < Storage::viewCatCount(); i++) {
    if (Storage::viewCatActual(i) == actual) return true;
  }
  return false;
}

static void applyParentalConfig() {
  applyContentFilters();

  int v = Parental::clampVolume((int)Session::volume());
  if (v != (int)Session::volume()) Session::setVolume((uint8_t)v);
  Player::setVolume(v);

  if (Parental::allowlistEnabled()) {
    int ci = Storage::currentCategory();
    if (ci >= 0 && !categoryVisible(ci) && s_screen != Screen::Locked) {
      if (Story::active()) {
        Story::stop();
        showStoryList();
      } else {
        Player::stop();
        showMusicList();
      }
    }
    int si = Story::currentIndex();
    if (si >= 0 && !Parental::storyAllowed(si)) {
      Story::stop();
      showStoryList();
    }
  }

  /* 白名单变化后可见列表可能整体改变, 当前列表页需要重绘 */
  if (s_screen == Screen::MusicList) {
    showMusicList();
  } else if (s_screen == Screen::StoryList || s_screen == Screen::StoryError) {
    showStoryList();
  } else if (s_screen == Screen::StoryCategoryList) {
    showStoryCategoryList();
  }
}

/* 音量: Session 是唯一真值源 (开机从 FRAM 恢复), 用绝对值下发避免与异步状态漂移 */
static void changeVolume(int delta) {
  uint8_t v = (uint8_t)constrain((int)Session::volume() + delta, 0, VOL_MAX);
  v = (uint8_t)Parental::clampVolume(v);
  Player::setVolume(v);
  Session::setVolume(v);            // 变化时落盘 (3 字节小记录, 拧旋钮也不拖慢 UI)
}

/* 把"当前分类 + 当前曲目 uuid"写进 FRAM; 内容未变时 Session 内部直接跳过。 */
static void rememberPosition() {
  PlayerStatus st = Player::status();
  int ci = Storage::currentCategory();
  const char* cat = "";
  if (ci > 0) cat = Storage::catName(ci);   // 0 = 全部歌曲, 存空串
  uint32_t id = (st.index >= 0)
              ? Storage::trackId(Storage::viewTrackGlobal(st.index))
              : 0;
  Session::setPosition(cat, id);
}

static void enterSettings() {
  s_settingsBack = s_screen;        // 记录来源屏, 保存后返回该屏 (主页进入 -> Home)
  s_screen = Screen::Settings;
  s_sel    = 0;
  Ui::drawSettings(Settings::data(), s_sel);
  Ui::settingsMessage("双击OK保存回主页  刷新会停止播放");
}

static void exitSettings() {
  Settings::save();                 // 保存到 FRAM
  if (s_settingsBack == Screen::StoryPlay) {
    s_screen      = Screen::StoryPlay;   // 返回故事播放页
    s_storyRedraw = true;                // 插画+信息条由下一帧整体重绘
  } else if (s_settingsBack == Screen::StoryAudio) {
    s_screen      = Screen::StoryAudio;  // 返回音频故事播放页
    s_audioRedraw = true;
  } else if (s_settingsBack == Screen::MusicList) {
    showMusicList();
  } else if (s_settingsBack == Screen::TrackList) {
    showTrackList();
  } else if (s_settingsBack == Screen::StoryList || s_settingsBack == Screen::StoryError) {
    showStoryList();
  } else if (s_settingsBack == Screen::StoryCategoryList) {
    showStoryCategoryList();
  } else if (s_settingsBack == Screen::NoContent) {
    showNoContent();
  } else {
    goHome();                       // 默认回主页
  }
}

/* 调整设置项: 0=亮度, 1=旋转, 2=EQ; 4..6 为开关 (旋钮向上开/向下关) */
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
  } else if (item == 2) {
    int e = ((int)s.eq + (delta >= 0 ? 1 : -1) + (int)EqPreset::COUNT) % (int)EqPreset::COUNT;
    s.eq = (EqPreset)e;
    Player::setEq(s.eq);
    Ui::refreshSettings(s, item);
  } else if (item == 3) {                       // 播放中: 空闲多久熄屏
    s.screenOffMin = Settings::cycleMinutes(s.screenOffMin, delta);
    Settings::save();
    Ui::refreshSettings(s, item);
  } else if (item == 4) {                       // 无播放: 空闲多久休眠
    s.sleepMin = Settings::cycleMinutes(s.sleepMin, delta);
    Settings::save();
    Ui::refreshSettings(s, item);
  } else if (item >= 6 && item <= 8) {
    uint8_t on = (delta > 0) ? 1 : 0;
    if (item == 6) {
      s.bleEnabled = on;
      Ble::setAdvertisingEnabled(on != 0);
    } else if (item == 7) {
      s.wifiEnabled = on;
      Network::apply(s.wifiEnabled != 0, s.apEnabled != 0);
    } else if (item == 8) {
      s.apEnabled = on;
      Network::apply(s.wifiEnabled != 0, s.apEnabled != 0);
    }
    Settings::save();               // 无线开关立即落盘
    Ui::refreshSettings(s, item);
  }
}

static void toggleSetting(int item) {
  SettingsData& s = Settings::data();
  if (item == 6) {
    s.bleEnabled = s.bleEnabled ? 0 : 1;
    Ble::setAdvertisingEnabled(s.bleEnabled != 0);
  } else if (item == 7) {
    s.wifiEnabled = s.wifiEnabled ? 0 : 1;
    Network::apply(s.wifiEnabled != 0, s.apEnabled != 0);
  } else if (item == 8) {
    s.apEnabled = s.apEnabled ? 0 : 1;
    Network::apply(s.wifiEnabled != 0, s.apEnabled != 0);
  }
  Settings::save();
  Ui::refreshSettings(s, item);
}

static void refreshCatalog() {
  Ui::boot("正在刷新列表", "将停止播放，请勿拔卡");
  Display::present(true);
  Story::stop();
  // STOP acknowledgement also drains earlier playback commands. Do not mutate
  // Storage's String arrays while the audio task may still hold their pointers.
  if (!Player::stopAndWait()) {
    Ui::drawSettings(Settings::data(), s_sel);
    Ui::settingsMessage("停止播放失败，请重试");
    return;
  }
  auto result = Catalog::refresh();
  applyContentFilters();
  s_musicSel = s_trackSel = s_storySel = 0;
  s_trackPlaying = -1;
  Input::encoderSteps(); // 丢弃扫描期间积累的旋钮步数
  Ui::drawSettings(Settings::data(), s_sel);
  if (result == Catalog::Result::Saved) {
    if (Storage::totalCount() >= MAX_TRACKS || Story::count() >= MAX_STORIES ||
        Storage::catCount() - 1 >= MAX_CATEGORIES) {
      Ui::settingsMessage("已保存，列表数量已达上限");
      return;
    }
    char message[80];
    snprintf(message, sizeof(message), "已保存：音乐%d 故事%d", Storage::totalCount(), Story::count());
    Ui::settingsMessage(message);
  } else if (result == Catalog::Result::SaveFailed) {
    Ui::settingsMessage("列表已更新，索引保存失败");
  } else {
    Ui::settingsMessage("扫描失败，请检查SD卡");
  }
}

/* ---------------- 音乐分类 ---------------- */

static void showNoContent() {
  s_screen = Screen::NoContent;
  Ui::drawNoContent();
}

/* 空列表提示: 主页入口进去后没有内容时给出放文件的路径, OK / 长按 SET 回主页 */
static void showEmpty(const char* title, const char* line1, const char* line2) {
  s_screen = Screen::Empty;
  Ui::drawNotice(title, line1, line2);
}

static void showBindCode(Screen back) {
  s_bindBack = back;
  s_bindUntil = millis() + 15000;
  s_screen = Screen::Bind;
  Ui::drawBindCode(Parental::bindCode());
  Serial.printf("[PCTL] bind code %s\n", Parental::bindCode());
}

static void restoreBindBack() {
  switch (s_bindBack) {
    case Screen::Settings:
      s_screen = Screen::Settings;
      Ui::drawSettings(Settings::data(), s_sel);
      break;
    case Screen::StoryPlay:
      s_screen = Screen::StoryPlay;
      s_storyRedraw = true;
      break;
    case Screen::StoryAudio:
      s_screen      = Screen::StoryAudio;   // 返回音频故事播放页
      s_audioRedraw = true;
      break;
    case Screen::VideoPlay:
      s_screen     = Screen::VideoPlay;     // 视频已在显示绑定码前暂停
      s_videoActAt = millis();
      break;
    case Screen::VideoList:
      showVideoList();
      break;
    case Screen::TrackList:
      showTrackList();
      break;
    case Screen::StoryList:
    case Screen::StoryError:
      showStoryList();
      break;
    case Screen::StoryCategoryList:
      showStoryCategoryList();
      break;
    case Screen::NoContent:
      showNoContent();
      break;
    case Screen::Main:
      s_screen = Screen::Main;
      Ui::drawStatic();
      Ui::update(Player::status(), Hall::detected());
      break;
    default:
      goHome();
      break;
  }
}

/* 绘制音乐分类列表 (置顶"全部歌曲" + /music 一级子文件夹, 受家长白名单过滤) */
static void showMusicList() {
  int n = Storage::viewCatCount();
  if (n == 0) {
    if (Parental::allowlistEnabled()) showNoContent();
    else showEmpty("还没有音乐", "把 MP3 文件放进 /music", "然后在设置中刷新列表");
    return;
  }
  if (s_musicSel >= n) s_musicSel = 0;
  if (s_musicSel < 0) s_musicSel = 0;
  s_screen = Screen::MusicList;
  Ui::drawMusicList(n, s_musicSel);
}

/* 播放当前分类中的第 i 首, 并回到播放界面 */
static void playMusicIndex(int i) {
  if (!Parental::canPlay()) return;
  s_trackSel = i;
  Player::playIndex(i, true);
  s_screen = Screen::Main;
  Ui::drawStatic();
  Ui::update(Player::status(), Hall::detected());
}

/* 绘制当前分类的歌曲列表 (正在播放的曲目高亮), 无曲目则回分类列表。 */
static void showTrackList() {
  int n = Storage::count();
  if (n == 0) { showEmpty("这个分类还没有歌", "请添加 MP3 后刷新列表", nullptr); return; }
  PlayerStatus st = Player::status();
  s_trackPlaying = (st.index >= 0 && st.index < n) ? st.index : 0;
  s_trackSel     = s_trackPlaying;
  s_screen = Screen::TrackList;
  Ui::drawTrackList(n, s_trackSel, s_trackPlaying);
}

/* ---------------- 视频模式 ---------------- */

/* /video 列表: 进入时重新扫描目录, 新拷入的视频不用刷新整卡索引 */
static void showVideoList() {
  int n = Video::scan();
  if (n == 0) {
    showEmpty("还没有视频", "把 MJPEG AVI 放进 /video", "用 tools/build_video.mjs 转换");
    return;
  }
  if (s_videoSel >= n) s_videoSel = 0;
  if (s_videoSel < 0)  s_videoSel = 0;
  const int playing = Video::active() ? Video::index() : -1;
  s_screen = Screen::VideoList;
  Ui::drawVideoList(n, s_videoSel, playing);
}

/* 停止视频并回到列表 (返回后列表照常可用) */
static void stopVideoToList() {
  Video::stop();
  showVideoList();
}

/* 暂停/继续: 画面计时和音轨必须同时停, 否则恢复后会对不上 */
static void toggleVideoPause() {
  if (!Video::active()) return;
  const bool next = !Video::paused();
  Video::pause(next);
  if (Video::hasAudio()) Player::toggle();
  s_videoActAt = millis();
}

/* 打开并起播第 i 个视频: 解析 AVI 索引可能耗时, 先提交等待页 */
static void playVideo(int i) {
  if (!Parental::canPlay()) return;
  const int n = Video::count();
  if (i < 0 || i >= n) return;
  s_videoSel = i;

  if (Story::active()) Story::stop();
  Ui::drawVideoLoadingBegin(Video::title(i));
  Display::present(true);

  if (!Video::open(i)) {
    s_screen = Screen::VideoError;
    Ui::boot("视频无法播放", Video::errorText());
    return;
  }

  Player::setAutoNext(false);              // 视频只放一遍, 结束由本文件处理
  if (Video::hasAudio()) Player::playPath(Video::audioPath(), true);
  else                   Player::stop();   // 静音视频: 别把上一首音乐留在后台

  Video::start();
  s_screen       = Screen::VideoPlay;
  s_videoStartAt = millis();
  s_videoActAt   = s_videoStartAt;
  Ui::drawVideoBar(0, Video::durationMs(), Player::status().volume, false, true, true);
  Serial.printf("[VID] play #%d %s (%d frames, %d ms)%s\n", i, Video::name(i),
                Video::frameCount(), Video::durationMs(),
                Video::hasAudio() ? "" : " [silent]");
}

/* 列表内切换上一个/下一个视频 */
static void switchVideo(int delta) {
  const int n = Video::count();
  if (n <= 0) return;
  int cur = Video::index();
  if (cur < 0) cur = s_videoSel;
  playVideo((cur + delta + n) % n);
}

/* 记忆里的分类是否就是 actual (0=全部歌曲) */
static bool sessionMatchesCategory(int actual) {
  const char* cat = Session::category();
  if (actual == 0) return cat[0] == 0;
  return cat[0] != 0 && strcmp(cat, Storage::catName(actual)) == 0;
}

/* 在当前播放列表里按记忆的 uuid 找上次那首 (-1 = 列表里没有) */
static int sessionTrackIndex() {
  uint32_t id = Session::trackId();
  return id ? Storage::findInPlaylist(id) : -1;
}

/* 选定可见分类 (0=全部歌曲) 并开始播放。
   若选的就是上次听的分类, 接着记忆里那首播; 否则 (或记忆里的曲目已不在列表) 从第一首开始。 */
static void startMusicCategory(int viewCi) {
  int ci = Storage::viewCatActual(viewCi);
  if (ci < 0 || !categoryVisible(ci)) return;

  /* 这个分类的播放列表已经在播放器里 (播放中或暂停): 直接回播放页。
     否则下面会重新选分类并起播, 从主页再进一次列表就会把正在放的歌
     从头重放一遍。 */
  PlayerStatus cur = Player::status();
  if (!Story::active() && cur.playing && Storage::currentCategory() == ci &&
      cur.index >= 0 && cur.index < Storage::count()) {
    s_musicSel     = viewCi;
    s_trackSel     = cur.index;
    s_trackPlaying = cur.index;
    s_screen       = Screen::Main;
    Ui::drawStatic();
    Ui::update(cur, Hall::detected());
    return;
  }

  if (!Storage::selectCategory(ci)) {          // 该分类无曲目: 停留列表
    Serial.printf("[MUS] category %d has no track\n", ci);
    return;
  }
  s_musicSel = viewCi;
  int start = 0;
  if (sessionMatchesCategory(ci)) {
    int r = sessionTrackIndex();
    if (r >= 0) start = r;
  }
  playMusicIndex(start);
}

/* ---------------- 开机恢复上次收听位置 ---------------- */

/* 音乐列表光标回到上次分类 (按名称匹配, 换卡/增删歌后不会错位)。
   分类不存在或被家长关闭时保持光标在"全部歌曲"。 */
static void restoreSessionSelection() {
  const char* cat = Session::category();
  if (!cat[0]) return;
  for (int i = 0; i < Storage::viewCatCount(); i++) {
    if (strcmp(Storage::viewCatName(i), cat) == 0) {
      s_musicSel = i;
      Serial.printf("[SES] category cursor -> %s\n", cat);
      return;
    }
  }
}

/* 开机恢复上次收听: 按 uuid 在记忆的分类里找到那首, 载入但不播放, 直接进播放页。
   分类/曲目已不存在 (换卡、删歌、被家长隐藏) 时返回 false, 由调用方回主页;
   分类还在但 uuid 匹配不到时, 从分类第一首开始。 */
static bool restoreSessionPlayback() {
  uint32_t id = Session::trackId();
  if (!id || !Parental::canPlay()) return false;      // 没有记忆或被家长限制

  const char* cat = Session::category();
  int view = -1;
  for (int i = 0; i < Storage::viewCatCount(); i++) {
    int actual = Storage::viewCatActual(i);
    bool match = (cat[0] == 0) ? (actual == 0)
                               : (strcmp(Storage::viewCatName(i), cat) == 0);
    if (match) { view = i; break; }
  }
  if (view < 0) return false;                         // 记住的分类不在了
  if (!Storage::selectCategory(Storage::viewCatActual(view))) return false;
  s_musicSel = view;

  int idx = sessionTrackIndex();
  if (idx < 0) idx = 0;                               // 列表里匹配不到 -> 从第一首开始

  s_trackSel     = idx;
  s_trackPlaying = idx;
  Player::playIndex(idx, false);                      // 载入但不自动播放
  s_screen = Screen::Main;
  Ui::drawStatic();
  Ui::update(Player::status(), Hall::detected());
  Serial.printf("[SES] resume cat=\"%s\" #%d id=%08x %s\n",
                cat, idx, (unsigned)id, Storage::path(idx) ? Storage::path(idx) : "");
  return true;
}

/* ---------------- 故事模式 ---------------- */

static int s_storyPanel = -1;            // 已绘制的插画下标

/* 控制条刷新: Ui 内部按变化区域增量重绘(含故事名跑马灯), 每帧调用开销极小 */
static void refreshStoryBar() {
  PlayerStatus st = Player::status();
  Ui::drawStoryBar(Story::panelIndex(), Story::panelCount(),
                   st.curSec, st.durSec, st.volume, st.paused);
}

/* 绘制 /story 一级分组: 音频故事 / 图文故事 */
static void showStoryCategoryList() {
  int n = Story::categoryCount();
  if (n == 0 || Story::viewCount() == 0) {
    if (Parental::allowlistEnabled()) showNoContent();
    else showEmpty("还没有故事", "把故事文件夹放进 /story", "然后在设置中刷新列表");
    return;
  }
  if (s_storyCatSel >= n) s_storyCatSel = 0;
  if (s_storyCatSel < 0) s_storyCatSel = 0;
  s_screen = Screen::StoryCategoryList;
  Ui::drawStoryCategoryList(n, s_storyCatSel);
}

/* 绘制某个故事目录的直接子项; path 可以是分类根, 也可以是任意层子目录 */
static void showStoryListAt(const String& path) {
  if (!Story::browse(path)) { showStoryCategoryList(); return; }

  int n = Story::browseCount();
  if (n == 0) {
    if (path == Story::categoryPath(s_storyCatSel)) {
      if (Parental::allowlistEnabled()) showNoContent();
      else showEmpty("这个分类还没有故事", "把故事文件夹放进对应分类", "然后在设置中刷新列表");
    } else {
      showEmpty("这个文件夹还没有故事", "继续放入故事文件夹", "长按 SET 回主页");
    }
    return;
  }

  s_storyPath = path;
  int current = Story::currentIndex();
  if (current >= 0) {
    for (int i = 0; i < n; ++i) {
      if (Story::browseIsStory(i) && Story::browseActual(i) == current) {
        s_storySel = i;
        break;
      }
    }
  }
  if (s_storySel >= n) s_storySel = 0;
  if (s_storySel < 0) s_storySel = 0;
  s_screen = Screen::StoryList;
  Ui::drawStoryList(s_storyPath.c_str(), n, s_storySel, current);
}

/* 当前播放故事所在目录; 没有播放时沿用上次浏览目录 */
static void showStoryList() {
  if (Story::active()) {
    String parent = Story::parentOfActual(Story::currentIndex());
    if (parent.length()) { showStoryListAt(parent); return; }
  }

  int cats = Story::categoryCount();
  if (cats == 0) { showStoryCategoryList(); return; }
  if (s_storyCatSel < 0 || s_storyCatSel >= cats) s_storyCatSel = 0;

  if (s_storyPath.isEmpty()) s_storyPath = Story::categoryPath(s_storyCatSel);
  showStoryListAt(s_storyPath);
}

/* 返回上一级目录; 已在分类根时回分类列表 */
static void storyBrowseUp() {
  if (Story::active()) Story::stop();
  String categoryRoot = Story::categoryPath(s_storyCatSel);
  if (s_storyPath.isEmpty() || s_storyPath == categoryRoot || s_storyPath == "/story") {
    showStoryCategoryList();
    return;
  }
  int k = s_storyPath.lastIndexOf('/');
  String parent = k <= 0 ? String("/story") : s_storyPath.substring(0, k);
  s_storySel = 0;
  showStoryListAt(parent);
}

/* 从故事列表/分类列表回到正在播放的页面; 播放不中断 */
static void returnToStoryPlayback() {
  if (!Story::active()) { showStoryList(); return; }
  int actual = Story::currentIndex();
  int cat = Story::categoryOfActual(actual);
  if (cat >= 0) s_storyCatSel = cat;
  if (Story::currentKind() == Story::Kind::Audio) {
    s_screen      = Screen::StoryAudio;
    s_audioRedraw = true;
  } else {
    s_screen      = Screen::StoryPlay;
    s_storyRedraw = true;
  }
}

/* 当前目录内切换上一个/下一个故事 (跳过子文件夹) */
static void switchStory(int delta) {
  int actual = Story::currentIndex();
  if (actual < 0) return;
  String parent = Story::parentOfActual(actual);
  if (parent.isEmpty() || !Story::browse(parent)) return;

  int n = Story::browseCount();
  if (n <= 0) return;
  int pos = -1;
  for (int i = 0; i < n; ++i) {
    if (Story::browseIsStory(i) && Story::browseActual(i) == actual) { pos = i; break; }
  }
  if (pos < 0) return;

  int next = pos;
  for (int step = 1; step <= n; ++step) {
    int cand = (pos + delta * step % n + n) % n;
    if (Story::browseIsStory(cand)) { next = cand; break; }
  }
  if (next == pos) return;

  int cat = Story::categoryOfActual(actual);
  if (cat >= 0) s_storyCatSel = cat;
  s_storyPath = parent;
  s_storySel = next;
  playStoryActual(Story::browseActual(next));
}

/* 打开一个实际故事下标 (加载页 -> 图文/音频播放页) */
static void playStoryActual(int actual) {
  if (actual < 0 || !Parental::storyAllowed(actual) || !Parental::canPlay()) return;

  int cat = Story::categoryOfActual(actual);
  if (cat >= 0) s_storyCatSel = cat;

  String parent = Story::parentOfActual(actual);
  s_storyPath = parent;
  if (Story::browse(parent)) {
    int leafIndex = 0, leafCount = 0;
    for (int i = 0; i < Story::browseCount(); ++i) {
      if (!Story::browseIsStory(i)) continue;
      if (Story::browseActual(i) == actual) leafIndex = leafCount;
      leafCount++;
      if (Story::browseActual(i) == actual) s_storySel = i;
    }
    s_storyPlayIndex = leafIndex;
    s_storyPlayCount = leafCount > 0 ? leafCount : 1;
  }

  Story::Kind kind = Story::kind(actual);

  /* 按下确定立刻进入加载画面; 真实的目录/时间轴读取就在加载页显示期间完成
     (此时列表页没有音频播放, 阻塞是安全的) */
  String name = Story::title(actual);
  Ui::setStoryTitle(name.c_str());          // 控制条显示的故事名
  Ui::drawStoryLoadingBegin(name.c_str());
  Display::present(true);          // 在同步读取目录前提交加载页
  if (!Story::open(actual)) {
    s_screen = Screen::StoryError;
    Ui::boot("故事文件不可用", "长按SET回主页 · 设置中刷新列表");
    return;
  }

  Story::start();

  if (kind == Story::Kind::Audio) {          // 音频故事: 复用音乐播放器布局
    s_screen      = Screen::StoryAudio;
    s_audioRedraw = true;
    return;
  }

  s_storyPanel = Story::panelIndex();
  s_screen     = Screen::StoryPlay;
  Ui::drawStoryFrame(Story::panelPath());
  Ui::invalidateStoryBar();          // 插画覆盖了控制条区, 需整体重铺
  refreshStoryBar();
}

/* 当前目录中的第 view 项: 文件夹下钻, 故事开始播放 */
static void startStory(int view) {
  if (view < 0 || view >= Story::browseCount()) return;
  if (Story::browseIsStory(view)) {
    playStoryActual(Story::browseActual(view));
    return;
  }
  String child = s_storyPath;
  if (!child.endsWith("/")) child += "/";
  child += Story::browseName(view);
  s_storySel = 0;
  showStoryListAt(child);
}

/* BLE 下发的是实际故事下标: 定位到它所在目录并播放 */
static void startStoryActual(int actual) {
  int cat = Story::categoryOfActual(actual);
  if (cat < 0) return;
  s_storyCatSel = cat;
  String parent = Story::parentOfActual(actual);
  if (parent.length()) {
    s_storyPath = parent;
    Story::browse(parent);
    for (int i = 0; i < Story::browseCount(); ++i) {
      if (Story::browseIsStory(i) && Story::browseActual(i) == actual) { s_storySel = i; break; }
    }
  }
  playStoryActual(actual);
}

/* BLE 下发的播放控制: 在 main loop 里执行, 保证 UI/Player 访问在同一任务 */
static void handleBleCommand(const Ble::Command& c) {
  if (FileXfer::busy()) return;

  /* 视频播放中: 手机端的播放/暂停/切换同样作用在视频上, 否则音轨会和画面对不上。
     手机点播故事/音乐时先停掉视频, 再走下面的原有逻辑。 */
  if (Video::active()) {
    switch (c.action) {
      case Ble::Action::Play:   if (Video::paused())  toggleVideoPause(); return;
      case Ble::Action::Pause:  if (!Video::paused()) toggleVideoPause(); return;
      case Ble::Action::Toggle: toggleVideoPause(); return;
      case Ble::Action::Next:   switchVideo(1);  return;
      case Ble::Action::Prev:   switchVideo(-1); return;
      case Ble::Action::Stop:   Video::stop(); showVideoList(); return;
      case Ble::Action::PlayStory:
      case Ble::Action::PlayMusic:
        Video::stop();
        s_screen = Screen::VideoList;
        break;
      default: break;
    }
  }

  switch (c.action) {
    case Ble::Action::Volume:
      setVolumeLimited(c.value);
      break;

    case Ble::Action::Play:
      if (!Parental::canPlay()) break;
      if (Player::paused()) Player::toggle();
      break;

    case Ble::Action::Pause:
      if (!Player::paused()) Player::toggle();
      break;

    case Ble::Action::Toggle:
      if (Parental::canPlay()) Player::toggle();
      break;

    case Ble::Action::Next:
    case Ble::Action::Prev: {
      if (!Parental::canPlay()) break;
      if (Story::active()) {
        switchStory(c.action == Ble::Action::Next ? 1 : -1);
      } else if (c.action == Ble::Action::Next) {
        Player::next();
      } else {
        Player::prev();
      }
      break;
    }

    case Ble::Action::Stop:
      if (Story::active()) {
        Story::stop();
        showStoryList();
      } else {
        Player::stop();
        showMusicList();
      }
      break;

    case Ble::Action::PlayStory:
      startStoryActual(c.value);
      break;

    case Ble::Action::PlayMusic: {
      int actual = c.value;
      int ci = Storage::allTrackCategory(actual);
      if (!Parental::categoryAllowed(ci)) break;
      int playlistIndex = -1;
      if (!Storage::prepareTrack(actual, playlistIndex)) break;
      if (Story::active()) Story::stop();
      for (int i = 0; i < Storage::viewCatCount(); i++) {
        if (Storage::viewCatActual(i) == ci) { s_musicSel = i; break; }
      }
      playMusicIndex(playlistIndex);
      break;
    }

    default:
      break;
  }
}

static const char* lockTitle(Parental::Lock lk) {
  switch (lk) {
    case Parental::Lock::DailyLimit: return "今日故事时间已用完";
    case Parental::Lock::Bedtime:    return "该睡觉啦";
    case Parental::Lock::Remote:     return "家长已暂停";
    default:                         return "家长控制中";
  }
}

static void armLock(Parental::Lock lk) {
  if (s_screen != Screen::Locked) {
    if (s_screen == Screen::Home) {
      s_lockedBack = Screen::Home;               // 解锁后回主页
    } else if (s_screen == Screen::StoryPlay || s_screen == Screen::StoryAudio ||
               s_screen == Screen::StoryList || s_screen == Screen::StoryCategoryList ||
               s_screen == Screen::StoryError) {
      s_lockedBack = Screen::StoryList;
    } else if (s_screen == Screen::VideoPlay || s_screen == Screen::VideoList ||
               s_screen == Screen::VideoError) {
      s_lockedBack = Screen::VideoList;
    } else {
      s_lockedBack = Screen::MusicList;
    }
    if (Story::active())      Story::stop();
    else if (Video::active()) Video::stop();
    else                      Player::stop();
    s_screen = Screen::Locked;
  }
  if (s_lockShown == lk) return;
  s_lockShown = lk;

  char l1[64] = {0};
  char l2[64] = {0};
  if (lk == Parental::Lock::DailyLimit) {
    snprintf(l1, sizeof(l1), "今日额度已用完");
    snprintf(l2, sizeof(l2), "明天 00:00 恢复 · 家长可调整额度");
    Ui::drawLock(lockTitle(lk), l1, l2);
  } else if (lk == Parental::Lock::Bedtime) {
    snprintf(l1, sizeof(l1), "休息时间 %02u:%02u - %02u:%02u",
             (unsigned)(Parental::bedtimeStartMin() / 60),
             (unsigned)(Parental::bedtimeStartMin() % 60),
             (unsigned)(Parental::bedtimeEndMin() / 60),
             (unsigned)(Parental::bedtimeEndMin() % 60));
    snprintf(l2, sizeof(l2), "明天早上再见");
    Ui::drawLock(lockTitle(lk), l1, l2);
  } else {
    snprintf(l1, sizeof(l1), "故事机已锁定");
    snprintf(l2, sizeof(l2), "家长可在小程序中解锁");
    Ui::drawLock(lockTitle(lk), l1, l2);
  }
}

/* ---------------- setup / loop ---------------- */

#ifdef TFT_BL_SWEEP
/* 背光啸叫排查: 每 4 秒换一个调光频率, 屏幕显示当前值, 串口同步打印。
   用法: platformio.ini 的 build_flags 加 -DTFT_BL_SWEEP, 烧录后听哪个频率最安静,
   再把 -DTFT_BL_FREQ=<那个值> 固定下来。 */
static void runBacklightSweep() {
  static const uint32_t kFreqs[] = {1000, 2000, 4000, 5000, 8000, 12000,
                                    16000, 20000, 25000, 30000, 35000};
  Display::setBrightness(60);          // 100% 时没有 PWM, 听不出问题
  for (uint32_t hz : kFreqs) {
    Display::setBacklightFreq(hz);
    char b[24];
    snprintf(b, sizeof(b), "%u Hz", (unsigned)hz);
    auto& g = Display::canvas();
    g.fillScreen(TFT_BLACK);
    g.setFont(&lgfx::fonts::Font4);
    g.setTextColor(TFT_WHITE, TFT_BLACK);
    g.drawCenterString(b, 160, 92);
    g.setFont(&lgfx::fonts::Font2);
    g.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    g.drawCenterString("listen for the whine", 160, 126);
    Display::present(true);
    Serial.printf("[BL] sweep %u Hz\n", (unsigned)hz);
    delay(4000);
  }
  Display::setBacklightFreq(TFT_BL_FREQ);
  Display::setBrightness(Settings::data().brightness);
  Serial.println("[BL] sweep done");
}
#endif

void setup() {
  Serial.setRxBufferSize(32768);    // 上位机文件传输分块较大
  Serial.setTxBufferSize(32768);
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32-S3 Music Player ===");
  Serial.printf("[SYS] PSRAM %s, size: %u bytes\n",
                psramFound() ? "OK" : "NOT FOUND", ESP.getPsramSize());

  Display::begin();

  Fram::begin();                    // I2C FRAM (设置持久化)
  Serial.printf("[SYS] FRAM %s\n", Fram::ok() ? "OK" : "NOT FOUND");

  /* PN532 与 FRAM 共用 I2C; 初始化失败不阻塞设备启动, 运行中会自动重试 */
  Nfc::begin();
  Serial.printf("[NFC] PN532 %s%s%s\n",
                Nfc::modulePresent() ? "OK" : "NOT FOUND",
                Nfc::modulePresent() ? ", fw " : "",
                Nfc::modulePresent() ? Nfc::firmwareVersion() : "");

  Settings::load();                 // 读取掉电保存的配置
  Session::load();                  // 读取掉电保存的音量/上次播放位置
  Parental::begin();                // 家长策略 + 今日用量 + 绑定信息
  Favorites::begin();               // 收藏位图 (FRAM)
#ifdef SESSION_SELFTEST
  Session::selfTest();              // 临时自检开关: -DSESSION_SELFTEST
#endif
  Serial.printf("[SES] FRAM 记忆: volume=%u, cat=\"%s\"\n",
                Session::volume(), Session::category());
  applySettings();

  Fonts::begin();                   // 内置 efont 中文点阵字体
  Ui::begin();
  Ui::boot("MUSIC PLAYER", "Mounting TF card ...");
  Display::present(true);

  Input::begin();
  Hall::begin();
  Serial.printf("[SYS] HALL level=%d (%s)\n",
                Hall::level(), Hall::detected() ? "card in" : "card out");
  Ui::setHall(Hall::detected());     // 顶栏通知位: 卡片插入状态
  Power::begin();                    // 电源管理: 记下起始空闲时刻 + 打印唤醒原因

  if (!Storage::begin()) {
    Ui::boot("SD CARD ERROR", "Insert TF card then reset");
  } else {
    auto result = Catalog::begin();
    Video::scan();                     // /video 不进媒体索引: 每次启动单独扫, 缺目录会创建
    if (result == Catalog::Result::ScanFailed) {
      Ui::boot("列表读取失败", "请在设置中刷新列表");
    } else if (result == Catalog::Result::SaveFailed) {
      Ui::boot("索引保存失败", "本次列表可用，请检查SD卡");
    } else if (Storage::totalCount() == 0 && Story::count() == 0 && Video::count() == 0) {
      Ui::boot("NO MEDIA FOUND", "Copy files to /music, /story or /video");
    } else {
      Ui::boot("MUSIC PLAYER", "Ready - press OK to select");
    }
    applyContentFilters();           // 家长白名单视图
    Serial.printf("[SD ] %d folder(s):", Storage::catCount() - 1);
    for (int i = 1; i < Storage::catCount(); i++)
      Serial.printf(" %s(%d)", Storage::catName(i), Storage::catTrackCount(i));
    Serial.printf(" | %d video(s)\n", Video::count());
  }
  Display::present(true);
  delay(600);

  Player::begin();
  Player::setEq(Settings::data().eq);
  setVolumeLimited(Session::volume());   // 恢复上次音量, 并套用家长音量范围
  Ble::begin(Settings::data().bleEnabled != 0);
  Network::begin(Settings::data().wifiEnabled != 0, Settings::data().apEnabled != 0);
  HostLink::begin();                // USB CDC 上位机协议 (NFC 读卡/写卡)

#ifdef RADIO_SELFTEST
  Serial.println("[TEST] radio selftest: WiFi+AP on");
  Network::apply(true, true);
  delay(6000);
  Serial.println("[TEST] radio selftest: BLE advertising off/on");
  Ble::setAdvertisingEnabled(false);
  delay(1000);
  Ble::setAdvertisingEnabled(true);
  delay(1000);
  Network::apply(false, false);
  Serial.println("[TEST] radio selftest done");
#endif

  if (!Parental::bound()) {          // 首次启动显示绑定码, 手机连接时也会再次显示
    Serial.printf("[PCTL] bind code %s\n", Parental::bindCode());
    Ui::drawBindCode(Parental::bindCode());
    Display::present(true);
    delay(4000);
  }

  restoreSessionSelection();        // 音乐列表光标回到上次分类
#ifdef TFT_BL_SWEEP
  runBacklightSweep();              // 临时: 背光频率扫频, -DTFT_BL_SWEEP
#endif
  /* 有可用的收听记忆就直接回播放页 (曲目已载入但暂停, 按 OK 继续); 否则回主页 */
  if (!restoreSessionPlayback()) showHome();
#ifdef POWER_TEST_AUTOPLAY
  /* 临时: 自动开播, 用于验证"播放中只熄屏" */
  if (Storage::count() > 0) {
    Player::playIndex(0, true);
    s_screen = Screen::Main;
    Ui::drawStatic();
    Ui::update(Player::status(), Hall::detected());
    Serial.println("[TEST] autoplay started");
  }
#endif
}

void loop() {
  // 每帧结束时主动让出 1ms，既给音频任务/idle 留出 CPU，也让上位机能
  // 从"执行时间 / 总时间"估算主循环占用率。传输期间不让出，避免拖慢 SD。
  struct FrameScope {
    FrameScope() { SysMon::frameBegin(); }
    ~FrameScope() { SysMon::frameEnd(!FileXfer::busy()); }
  } frameScope;

  // 即使页面处理提前 return，也在所有绘制完成后提交；输入仍可高速轮询。
  // 熄屏时跳过推屏: 省下每秒几十次整屏 SPI 传输。
  struct PresentOnExit {
    ~PresentOnExit() { if (!Power::screenOff()) Display::present(); }
  } presentOnExit;
  /* 解码在独立 FreeRTOS 音频任务里持续运行, 这里只处理界面与输入 */

  Ui::setHall(Hall::detected());    // 卡片插拔: 任意界面都即时更新顶栏通知位
  Network::loop();                  // WiFi STA 连接状态日志
  HostLink::loop();                 // USB CDC: PN532 轮询 / 上位机命令

  static bool transferShown = false;
  static uint32_t transferUiAt = 0;
  static int transferPercent = -1;
  if (FileXfer::busy()) {
    Power::loop(false);
    Ble::loop();
    Ble::Command ignored;
    while (Ble::takeCommand(ignored)) {}
    Input::encoderSteps(); Input::nextEvent(); Input::prevEvent(); Input::encEvent();
    if (Input::okEvent() == 3) { FileXfer::abortAll(); return; }
    const int pct = FileXfer::progress();
    if (!transferShown || (pct != transferPercent && millis() - transferUiAt >= 250)) {
      if (!transferShown) Ui::boot("文件传输中", "长按 OK 取消");
      auto& g = Display::canvas();
      g.fillRect(40, 193, 240, 12, g.color565(220, 225, 220));
      g.fillRect(40, 193, pct * 240 / 100, 12, g.color565(64, 123, 103));
      transferShown = true; transferPercent = pct; transferUiAt = millis();
    }
    return;
  }
  if (transferShown) {
    transferShown = false; transferPercent = -1;
    showHome(); // 文件可能已被替换，停止后回主页，不恢复旧文件句柄。
  }

  /* BLE 通道: 先收包/下发动作, 再由主循环统一执行 */
  Ble::loop();
  Ble::Command bleCmd;
  for (int i = 0; i < 4 && Ble::takeCommand(bleCmd); i++) handleBleCommand(bleCmd);

  PlayerStatus ps = Player::status();
  /* 视频可能没有音轨 (静音 AVI), 只按播放器状态判断会在看视频时误休眠 */
  const bool mediaPlaying = (ps.playing && !ps.paused) ||
                            (Video::active() && !Video::paused());
  Parental::tick(mediaPlaying);

  /* 电源管理: 播放中空闲只熄屏; 无播放空闲进深度休眠 (进去就不返回了)。
     熄屏后本函数会在下面直接 return, 界面分支里的按键读取都不会执行,
     所以必须先把输入扫一遍: 否则 Input::lastActivityMs() 永远不变,
     "播放中熄屏"后就再也收不到唤醒动作, 只能断电重启。 */
  if (Power::screenOff()) Input::poll();
  bool woke = Power::loop(mediaPlaying, Video::active() && !Video::paused());

  if (Ble::takeConfigChanged()) applyParentalConfig();

  /* 童锁: 只屏蔽实体按键/旋钮, BLE 控制始终保留.
     本地兜底解锁: OK + NEXT 同时按住 5 秒 (防止手机不在身边时被彻底锁死). */
  bool childLock = Parental::childLock();
  Input::setLocked(childLock);
  Ui::setChildLock(childLock);
  if (childLock && Input::childUnlockHeld()) {
    Parental::setChildLock(false);
    Input::setLocked(false);
    Ui::setChildLock(false);
    Serial.println("[PCTL] child lock released by OK+NEXT");
  }

  Parental::Lock lk = Parental::lock();
  if (lk != Parental::Lock::None) {
    armLock(lk);
    return;
  }
  if (s_screen == Screen::Locked) {
    s_lockShown = Parental::Lock::None;
    if      (s_lockedBack == Screen::StoryCategoryList) showStoryCategoryList();
    else if (s_lockedBack == Screen::StoryList) showStoryList();
    else if (s_lockedBack == Screen::VideoList) showVideoList();
    else if (s_lockedBack == Screen::Home)      goHome();
    else                                        showMusicList();
    return;
  }

  /* 手机首次连接时把绑定码显示到屏幕, 方便输入 */
  bool bleConnected = Ble::connected();
  if (bleConnected && !s_bleWasConnected && !Parental::bound() &&
      s_screen != Screen::Bind && s_screen != Screen::Locked) {
    if (Video::active() && !Video::paused()) toggleVideoPause();  // 别让画面在配对页后面空转
    showBindCode(s_screen);
  }
  s_bleWasConnected = bleConnected;

  /* 唤醒那一下只用来点亮屏幕, 不顺带执行按键功能 */
  if (woke) return;
  /* 熄屏期间不跑界面逻辑: 省电, 也避免误触 (输入已在上面由 Input::poll() 扫过,
     后台任务也都在上面跑过了) */
  if (Power::screenOff()) return;

  switch (s_screen) {

  /* ---------------- 主页 ---------------- */
  case Screen::Home: {
    /* 音量可能被 BLE 改动; 10Hz 足够, 避免每个 loop 都去拿播放状态锁 */
    static uint32_t lastHomeUi = 0;
    if (millis() - lastHomeUi >= 100) {
      lastHomeUi = millis();
      Ui::updateHome(s_homeSel, Player::status().volume);
    }

    int move = Input::encoderSteps();
    if (Input::nextEvent() == 1) move += 1;
    if (Input::prevEvent() == 1) move -= 1;
    if (move != 0) {
      s_homeSel = (int)((s_homeSel + move % 4 + 4) % 4);
      Ui::updateHome(s_homeSel, Player::status().volume);
      return;
    }

    int8_t enc = Input::encEvent();
    int8_t ok  = Input::okEvent();
    if (enc == 1 || ok == 1) {
      switch (s_homeSel) {
        case 0:  showMusicList();   break;
        case 1:  showStoryCategoryList(); break;
        case 2:  showVideoList(); break;
        default: enterSettings();   break;
      }
      return;
    }
    break;                                  // 主页长按 / 双击不执行动作
  }

  /* ---------------- 绑定码页 ---------------- */
  case Screen::Bind: {
    Input::encoderSteps();                 // 丢弃绑定页上的旋钮步数
    if (Parental::bound()) { restoreBindBack(); return; }
    if (!Ble::connected() && millis() > s_bindUntil) { restoreBindBack(); return; }
    if (Input::okEvent() == 2 || Input::encEvent() == 2) { restoreBindBack(); return; }
    break;
  }

  /* ---------------- 无内容 / 空列表 / 锁定占位 ---------------- */
  case Screen::NoContent:
  case Screen::Empty: {
    Input::encoderSteps();                       // 丢弃该页上的旋钮步数
    if (Input::encEvent() != 0 || Input::okEvent() == 1) { goHome(); return; }
    break;
  }

  case Screen::Locked:
    break;

  /* ---------------- 音乐分类列表 ---------------- */
  case Screen::MusicList: {
    int8_t enc = Input::encEvent();
    if (enc == 2) { goHome(); return; }                           // SET 长按: 回主页
    if (enc == 1) { startMusicCategory(s_musicSel); return; }     // SET 短按: 播放选中分类

    int move = Input::encoderSteps();
    if (Input::nextEvent() == 1) move += 1;
    if (Input::prevEvent() == 1) move -= 1;

    int n = Storage::viewCatCount();
    if (move != 0 && n > 0) {
      s_musicSel = (int)((s_musicSel + move % n + n) % n);
      Ui::updateMusicList(n, s_musicSel);
      return;
    }

    if (Input::okEvent() == 1) { startMusicCategory(s_musicSel); return; }
    break;
  }

  /* ---------------- 歌曲列表 ---------------- */
  case Screen::TrackList: {
    int n = Storage::count();
    if (n == 0) { showTrackList(); return; }

    int8_t enc = Input::encEvent();
    if (enc == 2) { goHome(); return; }                         // SET 长按: 回主页
    if (enc == 1) { playMusicIndex(s_trackSel); return; }        // SET 短按: 播放选中曲目

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
      case 1:                                                   // 单击: 播放选中曲目
        playMusicIndex(s_trackSel);
        return;
      case 3: {                                                 // 长按: 收藏/取消收藏
        int gi = Storage::viewTrackGlobal(s_trackSel);
        if (gi >= 0) {
          bool on = Favorites::toggle(gi);
          Serial.printf("[FAV] track %d -> %s\n", gi, on ? "on" : "off");
          Ui::drawTrackList(n, s_trackSel, s_trackPlaying);
        }
        return;
      }
      default: break;
    }

    /* 自动续播换曲时同步"播放中"高亮, 并更新 FRAM 记忆 */
    static uint32_t lastSync = 0;
    if (millis() - lastSync > 500) {
      lastSync = millis();
      int cur = Player::status().index;
      if (cur >= 0 && cur < n && cur != s_trackPlaying) {
        s_trackPlaying = cur;
        Ui::updateTrackList(n, s_trackSel, s_trackPlaying);
      }
      rememberPosition();
    }
    break;
  }

  /* ---------------- 视频列表 ---------------- */
  case Screen::VideoList: {
    int n = Video::count();
    if (n == 0) { showVideoList(); return; }

    int8_t enc = Input::encEvent();
    if (enc == 2) { goHome(); return; }                          // SET 长按: 回主页
    if (enc == 1) { playVideo(s_videoSel); return; }              // SET 短按: 播放选中视频

    int8_t nx = Input::nextEvent();
    int8_t pv = Input::prevEvent();
    if (nx == 2 || pv == 2) {                                     // 长按: 回到正在播放的视频
      if (Video::active()) {
        s_screen     = Screen::VideoPlay;
        s_videoActAt = millis();
      }
      return;
    }

    int move = Input::encoderSteps() + (nx == 1 ? 1 : 0) + (pv == 1 ? -1 : 0);
    if (move != 0) {
      s_videoSel = (int)((s_videoSel + move % n + n) % n);
      Ui::updateVideoList(n, s_videoSel, Video::active() ? Video::index() : -1);
      return;
    }

    if (Input::okEvent() == 1) { playVideo(s_videoSel); return; }
    break;
  }

  /* ---------------- 视频播放 ---------------- */
  case Screen::VideoPlay: {
    Video::loop();                                   // 到点就解码下一帧并画进画布

    if (Video::takeFinished()) {                     // 放完: 自动下一个, 末尾回列表
      const int n = Video::count();
      const int next = Video::index() + 1;
      if (next < n) playVideo(next);
      else          stopVideoToList();
      return;
    }

    int8_t enc = Input::encEvent();
    if (enc == 2) { Video::stop(); goHome(); return; }   // SET 长按: 停止并回主页
    if (enc == 1) { toggleVideoPause(); return; }        // SET 短按: 暂停/继续

    int8_t nx = Input::nextEvent();
    int8_t pv = Input::prevEvent();
    if (nx == 1) { switchVideo(1);  return; }            // 短按: 下一个视频
    if (pv == 1) { switchVideo(-1); return; }            // 短按: 上一个视频
    if (nx == 2) {                                       // 长按: 暂停并看列表
      if (!Video::paused()) toggleVideoPause();
      showVideoList();
      return;
    }
    if (pv == 2) { stopVideoToList(); return; }          // 长按 LAST: 停止回列表

    switch (Input::okEvent()) {
      case 1: toggleVideoPause(); break;                 // 单击: 暂停/继续
      case 3: stopVideoToList(); return;                 // 长按: 停止并返回列表
      default: break;
    }

    int steps = Input::encoderSteps();
    if (steps) { changeVolume(steps); s_videoActAt = millis(); }

    /* 控制条: 暂停时或操作后 4 秒内显示; 片名只在起播 3 秒内和暂停时显示 */
    const bool bar   = Video::paused() || (millis() - s_videoActAt < 4000);
    const bool title = bar && (Video::paused() || (millis() - s_videoStartAt < 3000));
    Ui::drawVideoStats(Video::actualFps(), Video::droppedFrames());
    Ui::drawVideoBar(Video::positionMs(), Video::durationMs(),
                     Player::status().volume, Video::paused(), bar, title);
    break;
  }

  /* ---------------- 视频打不开 ---------------- */
  case Screen::VideoError: {
    Input::encoderSteps();                            // 丢弃旋钮步数
    int8_t enc = Input::encEvent();
    if (enc == 2) { goHome(); return; }
    if (enc == 1 || Input::okEvent() == 1) { showVideoList(); return; }
    break;
  }

  /* ---------------- 音乐播放界面 ---------------- */
  case Screen::Main: {
    int8_t enc = Input::encEvent();
    if (enc == 2) { goHome(); return; }           // SET 长按: 回主页 (音乐继续播放)
    if (enc == 1) Player::toggle();               // SET 短按: 播放/暂停

    int8_t nx = Input::nextEvent();
    int8_t pv = Input::prevEvent();
    if (nx == 1)      Player::next();                       // 短按: 下一曲
    else if (nx == 2) { showTrackList(); return; }          // 长按: 当前歌曲列表
    if (pv == 1)      Player::prev();                       // 短按: 上一曲
    else if (pv == 2) { Player::stop(); showMusicList(); return; }   // 长按: 分类列表

    switch (Input::okEvent()) {
      case 1: Player::toggle(); break;    // 单击: 播放/暂停
      case 3: Player::stop(); showMusicList(); return;   // 长按: 停止并返回分类列表
      default: break;
    }

    int steps = Input::encoderSteps();
    if (steps) changeVolume(steps);

    static uint32_t lastUi = 0;
    if (millis() - lastUi >= 33) {
      lastUi = millis();
      Ui::update(Player::status(), Hall::detected());
      rememberPosition();           // 换曲/自动续播/选分类都在这里落到 FRAM
    }
    break;
  }

  /* ---------------- 设置界面 ---------------- */
  case Screen::Settings: {
    if (Input::nextEvent() == 1) { s_sel = (s_sel + 1) % SETTINGS_ITEMS; Ui::refreshSettings(Settings::data(), s_sel); }
    if (Input::prevEvent() == 1) { s_sel = (s_sel - 1 + SETTINGS_ITEMS) % SETTINGS_ITEMS; Ui::refreshSettings(Settings::data(), s_sel); }

    if (Input::encEvent() == 2) { Settings::save(); goHome(); return; }   // SET 长按: 保存回主页

    switch (Input::okEvent()) {
      case 1:
        if (s_sel == 5) { refreshCatalog(); return; }
        if (s_sel == 9) { showBindCode(Screen::Settings); return; }
        if (s_sel >= 6 && s_sel <= 8) { toggleSetting(s_sel); break; }
        adjustSetting(s_sel, +1); break;
      case 2: exitSettings();           return;  // 双击: 保存返回
      default: break;
    }

    int steps = Input::encoderSteps();
    if (steps) adjustSetting(s_sel, steps);
    break;
  }

  /* ---------------- 故事分类列表 ---------------- */
  case Screen::StoryCategoryList: {
    int8_t enc = Input::encEvent();
    if (enc == 2) { goHome(); return; }
    if (enc == 1) {
      s_storyPath = Story::categoryPath(s_storyCatSel);
      s_storySel = 0;
      showStoryListAt(s_storyPath);
      return;
    }

    int move = Input::encoderSteps();
    if (Input::nextEvent() == 1) move += 1;
    if (Input::prevEvent() == 1) move -= 1;

    int n = Story::categoryCount();
    if (move != 0 && n > 0) {
      s_storyCatSel = (int)((s_storyCatSel + move % n + n) % n);
      Ui::updateStoryCategoryList(n, s_storyCatSel);
      return;
    }

    if (Input::okEvent() == 1) {
      s_storyPath = Story::categoryPath(s_storyCatSel);
      s_storySel = 0;
      showStoryListAt(s_storyPath);
      return;
    }
    break;
  }

  /* ---------------- 故事目录浏览 ---------------- */
  case Screen::StoryList: {
    int n = Story::browseCount();
    if (n == 0) { storyBrowseUp(); return; }

    int8_t enc = Input::encEvent();
    if (enc == 2) { goHome(); return; }           // SET 长按: 回主页
    if (enc == 1) { startStory(s_storySel); return; }

    int8_t nx = Input::nextEvent();
    int8_t pv = Input::prevEvent();
    if (nx == 2 || pv == 2) {                     // 长按: 回到正在播放的页面
      if (Story::active()) returnToStoryPlayback();
      else                 storyBrowseUp();
      return;
    }

    int move = Input::encoderSteps() + (nx == 1 ? 1 : 0) + (pv == 1 ? -1 : 0);
    if (move != 0 && n > 0) {
      s_storySel = (int)((s_storySel + move % n + n) % n);
      Ui::updateStoryList(s_storyPath.c_str(), n, s_storySel, Story::currentIndex());
      return;
    }

    switch (Input::okEvent()) {
      case 1: startStory(s_storySel); return;
      case 3: storyBrowseUp(); return;
      default: break;
    }
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
    if (enc == 2) { goHome(); return; }           // SET 长按: 停止故事并回主页
    if (enc == 1) Player::toggle();               // SET 短按: 暂停/继续

    int8_t nx = Input::nextEvent();
    int8_t pv = Input::prevEvent();
    if (nx == 1) { switchStory(1); return; }       // 短按: 下一个故事
    if (pv == 1) { switchStory(-1); return; }      // 短按: 上一个故事
    if (nx == 2) { showStoryList(); return; }      // 长按: 当前分类播放列表
    if (pv == 2) { Story::stop(); showStoryCategoryList(); return; }

    switch (Input::okEvent()) {
      case 1: Player::toggle(); break;              // 单击: 暂停/继续
      case 3: Story::stop(); showStoryList();  return;   // 长按: 停止并返回列表
      default: break;
    }

    int steps = Input::encoderSteps();
    if (steps) changeVolume(steps);

    refreshStoryBar();                              // 每帧刷新, 内部只重绘变化区域
    break;
  }

  /* ---------------- 音频故事播放 ---------------- */
  case Screen::StoryAudio: {
    Story::loop();

    if (Story::takeFinished()) {                  // 播放完毕 -> 回列表
      Story::stop();
      showStoryList();
      return;
    }

    /* 与音乐播放器相同的静态布局 + 增量刷新。playPath 不会更新音乐列表下标,
       这里把状态映射成单曲故事信息, 避免显示上一首音乐的名字/计数。 */
    PlayerStatus st = Player::status();
    st.hasTrack = true;
    st.index = s_storyPlayIndex;
    st.count = s_storyPlayCount;
    st.name = Story::currentTitle();
    if (s_audioRedraw) {
      s_audioRedraw = false;
      int cat = Story::categoryOfActual(Story::currentIndex());
      const char* category = cat >= 0 ? Story::categoryName(cat) : "音频故事";
      Ui::drawStoryAudioStatic(category);
      Ui::update(st, Hall::detected());
    }
    static uint32_t lastAudioUi = 0;
    if (millis() - lastAudioUi >= 33) {
      lastAudioUi = millis();
      Ui::update(st, Hall::detected());
    }

    int8_t enc = Input::encEvent();
    if (enc == 2) { goHome(); return; }             // SET 长按: 停止故事并回主页
    if (enc == 1) { Player::toggle(); return; }      // SET 短按: 暂停/继续

    int8_t nx = Input::nextEvent();
    int8_t pv = Input::prevEvent();
    if (nx == 1) { switchStory(1); return; }
    if (pv == 1) { switchStory(-1); return; }
    if (nx == 2) { showStoryList(); return; }        // 长按: 当前分类播放列表
    if (pv == 2) { Story::stop(); showStoryCategoryList(); return; }

    switch (Input::okEvent()) {
      case 1: Player::toggle(); return;                              // 单击: 暂停/继续
      case 2: enterSettings(); return;                               // 双击: 进入设置
      case 3: Story::stop(); showStoryList(); return;                // 长按: 停止并返回列表
      default: break;
    }

    int steps = Input::encoderSteps();
    if (steps) changeVolume(steps);
    break;
  }

  /* ---------------- 故事提示页 ---------------- */
  case Screen::StoryError: {
    Input::encoderSteps();                        // 丢弃旋钮步数
    int8_t enc = Input::encEvent();
    if (enc == 2) { goHome(); return; }           // SET 长按: 回主页

    bool back = (enc == 1) || Input::nextEvent() == 1 || Input::prevEvent() == 1;
    if (Input::okEvent() == 1) back = true;

    if (back) {
      s_storySel = 0;
      showStoryList();
    }
    break;
  }

  }
}
