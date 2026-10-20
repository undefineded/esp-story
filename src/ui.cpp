/* UI 组件: 小小故事盒风格界面 (对照 ui.html 设计稿) + 设置界面 + 故事模式 */
#include <FS.h>          // 必须在 LovyanGFX 之前: 用来启用 drawJpgFile(fs, path) 重载
#include <SD_MMC.h>
#include "ui.h"
#include "config.h"
#include "display.h"
#include "fonts.h"
#include "render_math.h"
#include "storage.h"
#include "story.h"
#include <LovyanGFX.hpp>

/* 字体统一由 Fonts:: 提供原生尺寸 efont 点阵，前景/背景双色覆盖文字区域。 */

namespace {

/* ---------------- 故事盒调色板 (取自 ui.html) ---------------- */
uint16_t cInk, cInkSoft, cPaper, cCoral, cCoralDk, cYellow, cYellowLt,
         cAqua, cAquaDk, cBlue, cTrack, cTrackBd, cBtnBlue, cBtnBlueBd,
         cGreen, cGreenDk, cPauseBd, cPurple, cCoverBg, cMoon, cBush,
         cCardLt, cCardDk;
bool     s_palReady = false;

void palette() {
  if (s_palReady) return;
  s_palReady = true;
  auto& g = Display::canvas();
  cInk      = g.color565( 23,  50,  77);   // #17324d 深蓝墨色(描边/文字)
  cInkSoft  = g.color565( 91, 111, 130);   // #5b6f82 次要文字
  cPaper    = g.color565(255, 242, 176);   // #fff2b0 淡黄纸底
  cCoral    = g.color565(240,  90,  86);   // #f05a56 顶栏珊瑚红
  cCoralDk  = g.color565(217,  67,  67);   // #d94343 顶栏下边
  cYellow   = g.color565(255, 200,  61);   // #ffc83d 主强调黄
  cYellowLt = g.color565(255, 231, 154);   // #ffe79a 按钮描边黄
  cAqua     = g.color565( 43, 183, 170);   // #2bb7aa 分类徽章
  cAquaDk   = g.color565( 22, 143, 133);   // #168f85
  cBlue     = g.color565( 60, 120, 216);   // #3c78d8 进度蓝
  cTrack    = g.color565(212, 221, 225);   // #d4dde1 进度槽
  cTrackBd  = g.color565(164, 178, 185);   // #a4b2b9
  cBtnBlue  = g.color565( 49,  90, 130);   // #315a82 按钮蓝
  cBtnBlueBd= g.color565(111, 149, 184);   // #6f95b8
  cGreen    = g.color565( 73, 185,  92);   // #49b95c 播放点
  cGreenDk  = g.color565( 35, 123,  49);   // #237b31
  cPauseBd  = g.color565(169, 119,   0);   // #a97700 暂停点描边
  cPurple   = g.color565(114,  85, 166);   // #7255a6
  cCoverBg  = g.color565(124, 201, 221);   // #7cc9dd 封面底
  cMoon     = g.color565(255, 244, 168);   // #fff4a8 月亮
  cBush     = g.color565( 47, 155, 104);   // #2f9b68 灌木
  /* 霍尔卡片图标 (按设计稿 SVG 取色) */
  cCardLt = g.color565(191, 244, 186);     // #bff4ba 卡片本体 / 芯片内圆孔
  cCardDk = g.color565( 45, 206,  27);     // #2dce1b 芯片 / 右侧条
}

/* ---------------- 布局常量 (320x240, 对照 ui.html 1x) ----------------
   顶栏 y0..32 | 内容 y32..188 | 控制栏 y188..240
   封面卡 (10,42) 92x112, 硬阴影偏移 +3,+3 | 信息列 x112..310 */
const int INFO_X = 112, INFO_W = 198;

/* ---------------- 刷新缓存 (只重绘变化区域) ---------------- */
int      s_lastIdx = -999, s_lastCnt = -1, s_lastVol = -1;
int      s_lastState = -1;                 // playing/paused/hasTrack 组合键
int      s_lastHall = -1;
bool     s_hallNow  = false;               // 最近已知霍尔状态 (顶栏通知位, 各界面共用)
bool     s_hallLayout = false;             // 上次"音量让位"排版所用的霍尔状态
void     drawHallIcon(bool detected);      // 前置声明: 顶栏基础绘制里要用
uint32_t s_lastCur = 0xFFFFFFFF, s_lastDur = 0xFFFFFFFF;
String   s_subLast;                        // 已绘制的"编码·比特率"行内容 (比特率首帧解码后才给出, 变化时需补绘)

/* 超长曲名跑马灯状态：按时间推进，与调用频率解耦 */
String   s_ttl;                 // 完整曲名
int16_t  s_ttlW   = 0;         // 曲名完整宽度 px
int16_t  s_scroll = 0;         // 当前滚动偏移 px
uint32_t s_titleEpoch = 0;

String fit(const String& s, int maxW) {
  auto& g = Display::canvas();
  if (g.textWidth(s) <= maxW) return s;
  String t = s;
  while (t.length() > 0 && g.textWidth(t + "…") > maxW) t.remove(t.length() - 1);
  return t + "…";
}

String fmtSec(uint32_t s) {
  char b[16];
  snprintf(b, sizeof(b), "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  return String(b);
}

/* ---------------- 品牌笑脸 LOGO ---------------- */
void drawLogo(int cx, int cy, int r) {
  auto& g = Display::canvas();
  g.fillCircle(cx, cy, r, cYellow);
  g.drawCircle(cx, cy, r, TFT_WHITE);
  g.drawCircle(cx, cy, r - 1, TFT_WHITE);
  int er = r / 5; if (er < 2) er = 2;
  g.fillCircle(cx - r / 2, cy - r / 3, er, cInk);        // 眼睛
  g.fillCircle(cx + r / 2, cy - r / 3, er, cInk);
  int mw = r * 2 / 3;
  g.fillRect(cx - mw / 2, cy + r / 4, mw, 2, cInk);      // 微笑
  g.fillRect(cx - mw / 2 - 1, cy + r / 4 - 1, 2, 2, cInk);
  g.fillRect(cx + mw / 2 - 1, cy + r / 4 - 1, 2, 2, cInk);
}

/* ---------------- 顶栏 (珊瑚红) ---------------- */
void drawTopBarBase() {
  auto& g = Display::canvas();
  g.fillRect(0, 0, 320, 30, cCoral);
  g.fillRect(0, 30, 320, 2, cCoralDk);
  drawLogo(19, 15, 10);
  g.setFont(Fonts::body());
  g.setTextColor(TFT_WHITE, cCoral);
  g.drawString("小小故事盒", 36, 7);
  s_lastHall = -1;                  // 整条顶栏被重铺, 通知位图标必须重画
  drawHallIcon(s_hallNow);          // 通知位: 卡片插入状态 (各界面常显)
}

/* 音量文本 "音量:10" (顶栏右侧; 检测到卡片时右缘让给通知位图标) */
void drawVolume(const PlayerStatus& st) {
  auto& g = Display::canvas();
  const bool card = s_hallNow;               // 有卡片才需要给图标让位
  g.fillRect(card ? 228 : 238, 0, card ? 66 : 82, 30, cCoral);
  g.setFont(Fonts::body());
  g.setTextColor(TFT_WHITE, cCoral);
  char b[24];
  snprintf(b, sizeof(b), "音量:%u", (unsigned)st.volume);
  g.drawRightString(b, card ? 292 : 310, 7);
}

/* ---------------- 封面卡 (黄色相框 + 3px 硬阴影) ---------------- */
void drawCover() {
  auto& g = Display::canvas();
  g.fillRoundRect(13, 45, 92, 112, 6, cInk);     // 硬阴影 offset(3,3)
  g.fillRoundRect(10, 42, 92, 112, 6, cInk);     // 2px 墨色描边底
  g.fillRoundRect(12, 44, 88, 108, 5, cYellow);
  /* 封面插画: 蓝底 + 月亮 + 云朵 + 灌木 (对应 CSS placeholder) */
  g.fillRoundRect(14, 47, 82, 84, 2, cCoverBg);
  g.fillCircle(71, 73, 16, cMoon);
  g.drawCircle(71, 73, 16, cInk);
  g.drawCircle(71, 73, 15, cInk);
  g.fillCircle(37, 112, 8, cBush);
  g.fillCircle(70, 111, 8, cBush);
  g.fillRoundRect(29, 101, 48, 20, 10, TFT_WHITE);
  g.fillCircle(44, 101, 10, TFT_WHITE);
  g.fillCircle(60, 99, 12, TFT_WHITE);
  g.setFont(Fonts::body());
  g.setTextColor(cInk, cYellow);
  g.drawCenterString("音乐播放", 56, 135);
}

/* ---------------- 信息列 ---------------- */

/* 分类徽章 (静态): 宽度随文字自适应, 左右各 6px 内边距 (对照 ui.html .category 的 padding: 0 6px) */
void drawCategory() {
  auto& g = Display::canvas();
  const char* label = "音乐";
  const int BADGE_PAD = 6;
  g.setFont(Fonts::body());
  int w = g.textWidth(label) + BADGE_PAD * 2;
  g.fillRoundRect(INFO_X, 43, w, 18, 4, cAqua);
  g.drawRoundRect(INFO_X, 43, w, 18, 4, cAquaDk);
  g.setTextColor(TFT_WHITE, cAqua);
  g.drawString(label, INFO_X + BADGE_PAD, 44);
}

/* 集数 "03 / 24" */
void drawCount(const PlayerStatus& st) {
  int cnt = st.count, idx = st.index;
  if (cnt == s_lastCnt && idx == s_lastIdx) return;
  s_lastCnt = cnt; s_lastIdx = idx;

  auto& g = Display::canvas();
  g.fillRect(240, 43, 70, 18, cPaper);
  g.setFont(Fonts::small());
  g.setTextColor(cInkSoft, cPaper);
  char b[16];
  if (cnt > 0) snprintf(b, sizeof(b), "%02u / %02u", (unsigned)(idx + 1), (unsigned)cnt);
  else         snprintf(b, sizeof(b), "-- / --");
  g.drawRightString(b, 310, 47);
}

/* 曲名行: 超长时按 s_scroll 偏移绘制, 裁剪限制在信息列内 */
const int TITLE_H     = 22;    // 曲名行高 (16px 字体)
int scrollOffset(int width, int viewport, uint32_t epoch) {
  return RenderMath::scrollOffset(width, viewport, millis() - epoch);
}

void drawTitleLine() {
  auto& g = Display::canvas();
  g.setFont(Fonts::title());
  g.setTextColor(cInk, cPaper);
  g.setClipRect(INFO_X, 63, INFO_W, TITLE_H);
  g.fillRect(INFO_X, 63, INFO_W, TITLE_H, cPaper);
  g.drawString(s_ttl.c_str(), INFO_X - s_scroll, 65);
  g.clearClipRect();
}

/* 首尾各停两秒；仅当整数像素位置变化时绘制 */
void scrollTitle() {
  if (s_ttlW <= INFO_W || s_ttl.length() == 0) return;
  int offset = scrollOffset(s_ttlW, INFO_W, s_titleEpoch);
  if (offset == s_scroll) return;
  s_scroll = offset;
  drawTitleLine();
}

/* 副标题行文本: "MP3·128kbps" / "请插入 TF 卡" */
String subText(const PlayerStatus& st) {
  char b[48];
  if (st.hasTrack) snprintf(b, sizeof(b), "%s·%ukbps", st.codec ? st.codec : "--",
                            (unsigned)st.bitrateKbps);
  else             snprintf(b, sizeof(b), "请插入 TF 卡");
  return String(b);
}

/* 只重绘"编码·比特率"一行
   解码器要解到第一帧才知道比特率, 换曲时它通常还是 0, 因此这行需独立补绘 */
void drawSubLine(const PlayerStatus& st) {
  auto& g = Display::canvas();
  g.setFont(Fonts::body());
  s_subLast = subText(st);
  g.fillRect(INFO_X, 88, INFO_W, 24, cPaper);
  g.setTextColor(cInkSoft, cPaper);
  g.drawString(fit(s_subLast, INFO_W).c_str(), INFO_X, 95);
}

/* 标题 + 副标题 (整块重绘, 曲名超长时启动跑马灯) */
void drawTitleSub(const PlayerStatus& st) {
  auto& g = Display::canvas();
  g.fillRect(INFO_X, 63, INFO_W, 52, cPaper);

  g.setFont(Fonts::title());
  String nm = st.hasTrack ? String(st.name) : String("未发现音乐文件");
  s_ttl  = nm;
  s_ttlW = g.textWidth(nm);
  s_scroll = 0;
  s_titleEpoch = millis();
  drawTitleLine();

  drawSubLine(st);
}

/* 时间行 */
void drawTimes(const PlayerStatus& st) {
  auto& g = Display::canvas();
  g.fillRect(INFO_X, 116, INFO_W, 16, cPaper);
  g.setFont(Fonts::small());
  g.setTextColor(cInkSoft, cPaper);
  g.drawString(fmtSec(st.curSec).c_str(), INFO_X, 118);
  g.drawRightString(fmtSec(st.durSec).c_str(), 310, 118);
}

/* 进度条: 灰槽 + 蓝色填充 + 黄色端帽 */
void drawProgress(const PlayerStatus& st) {
  auto& g = Display::canvas();
  g.fillRect(INFO_X - 2, 132, INFO_W + 4, 12, cPaper);
  g.fillRoundRect(INFO_X, 134, INFO_W, 8, 2, cTrack);
  g.drawRoundRect(INFO_X, 134, INFO_W, 8, 2, cTrackBd);
  int wf = 0;
  if (st.durSec > 0) wf = (int)((uint64_t)(INFO_W - 2) * st.curSec / st.durSec);
  if (wf > 1) {
    if (wf > INFO_W - 2) wf = INFO_W - 2;
    g.fillRect(INFO_X + 1, 135, wf, 6, cBlue);
    if (wf > 4) g.fillRect(INFO_X + 1 + wf - 4, 135, 4, 6, cYellow);  // 黄色端帽
  }
}

/* 状态行: 圆点 + 播放状态 */
void drawStatus(const PlayerStatus& st) {
  auto& g = Display::canvas();
  g.fillRect(INFO_X, 142, INFO_W, 26, cPaper);

  uint16_t dot, border;
  const char* txt;
  if      (!st.hasTrack)             { dot = cTrack;  border = cTrackBd; txt = "无音乐"; }
  else if (st.playing && !st.paused) { dot = cGreen;  border = cGreenDk; txt = "正在播放"; }
  else if (!st.playing)              { dot = cYellow; border = cPauseBd; txt = "已停止"; }
  else                               { dot = cYellow; border = cPauseBd; txt = "已经暂停"; }

  g.fillCircle(INFO_X + 4, 154, 4, border);
  g.fillCircle(INFO_X + 4, 154, 3, dot);
  g.setFont(Fonts::body());
  g.setTextColor(cInkSoft, cPaper);
  g.drawString(txt, INFO_X + 14, 146);
}

/* 顶栏最右侧的霍尔图标 (卡片), 造型按设计稿 SVG 等比缩放:
   浅绿圆角卡片 + 左侧深绿芯片 (含浅绿圆孔) + 右侧深绿条。
   仅在检出实体卡片时显示; 没有卡片时什么都不画 (不占位, 音量文字自动回到最右缘)。
   1x 直绘 (不做抗锯齿), 卡片 18x13 贴在顶栏右缘。 */
void drawHallIcon(bool detected) {
  int v = detected ? 1 : 0;
  if (v == s_lastHall) return;               // 状态未变不重绘
  s_lastHall = v;

  auto& g = Display::canvas();
  const int X = 298, Y = 10;                 // 卡片左上角 (右缘留 4px)

  g.fillRect(X - 2, Y - 2, 22, 17, cCoral);  // 先清掉原图标位
  if (!detected) return;                     // 无霍尔: 不展示

  g.fillRoundRect(X,      Y,     18, 13, 3, cCardLt);   // 卡片本体
  g.fillRoundRect(X +  2, Y +  3,  8,  8, 2, cCardDk);  // 芯片
  g.fillCircle   (X +  6, Y +  7,      2,     cCardLt); // 芯片内圆孔
  g.fillRoundRect(X + 11, Y +  5,  5,  3, 1, cCardDk);  // 右侧条
}

/* ---------------- 控制栏 ---------------- */
void drawTransport() {
  auto& g = Display::canvas();
  g.fillRect(0, 188, 320, 52, cInk);
  g.fillRect(0, 188, 320, 3, cYellow);
  /* 上一曲 52x34: 竖条在左 + 三角尖朝左 (|◀, 与右侧 ▶| 镜像) */
  g.fillRoundRect(41, 197, 52, 34, 10, cBtnBlueBd);
  g.fillRoundRect(43, 199, 48, 30, 8, cBtnBlue);
  g.fillRect(61, 208, 3, 12, TFT_WHITE);
  g.fillTriangle(71, 208, 71, 220, 61, 214, TFT_WHITE);
  /* 下一曲 52x34 */
  g.fillRoundRect(227, 197, 52, 34, 10, cBtnBlueBd);
  g.fillRoundRect(229, 199, 48, 30, 8, cBtnBlue);
  g.fillRect(258, 208, 3, 12, TFT_WHITE);
  g.fillTriangle(249, 208, 249, 220, 259, 214, TFT_WHITE);
}

/* 主播放按钮 82x40: 黄底 + 图标 + 汉字标签 */
void drawPlayButton(const PlayerStatus& st) {
  auto& g = Display::canvas();
  g.fillRect(117, 192, 86, 46, cInk);
  g.fillRoundRect(119, 194, 82, 40, 12, cYellowLt);
  g.fillRoundRect(121, 196, 78, 36, 10, cYellow);

  bool pauseIcon = st.playing && !st.paused;
  int ix = 140, iy = 214;
  if (pauseIcon) {
    g.fillRect(ix - 7, iy - 9, 5, 18, cInk);
    g.fillRect(ix + 2, iy - 9, 5, 18, cInk);
  } else {
    g.fillTriangle(ix - 7, iy - 9, ix - 7, iy + 9, ix + 8, iy, cInk);
  }
  g.setFont(Fonts::body());
  g.setTextColor(cInk, cYellow);
  g.drawString(pauseIcon ? "暂停" : "播放", ix + 14, iy - 8);
}

/* ---------------- 设置界面 ---------------- */

const char* eqLabelCN(EqPreset p) {
  switch (p) {
    case EqPreset::Vocal: return "人声";
    case EqPreset::Pop:   return "流行";
    case EqPreset::Bass:  return "低音";
    default:              return "平直";
  }
}

/* 行卡片: 白底 + 描边; 选中: 黄底 + 墨色描边 + 3px 硬阴影 */
void settingsRow(int y, const char* label, const String& value, bool selected, int barPct) {
  auto& g = Display::canvas();
  if (selected) g.fillRoundRect(13, y + 3, 300, 44, 6, cInk);
  g.fillRoundRect(10, y, 300, 44, 6, selected ? cInk : TFT_WHITE);
  g.fillRoundRect(12, y + 2, 296, 40, 5, selected ? cYellow : TFT_WHITE);
  if (!selected) g.drawRoundRect(10, y, 300, 44, 6, cTrackBd);

  uint16_t bgRow = selected ? cYellow : TFT_WHITE;
  g.setFont(Fonts::body());
  g.setTextColor(cInk, bgRow);
  g.drawString(label, 24, y + 7);
  g.setTextColor(selected ? cInk : cInkSoft, bgRow);
  g.drawRightString(value.c_str(), 294, y + 7);

  if (barPct >= 0) {
    g.fillRoundRect(24, y + 26, 272, 8, 2, cTrack);
    g.drawRoundRect(24, y + 26, 272, 8, 2, cTrackBd);
    int w = 272 * barPct / 100;
    if (w > 1) {
      if (w > 272) w = 272;
      g.fillRect(25, y + 27, w - 2, 6, cBlue);
      if (w > 6) g.fillRect(25 + w - 2 - 4, y + 27, 4, 6, cYellow);
    }
  }
}

/* EQ 行频响示意 (蓝=提升 黄=衰减) */
void settingsBands(int y, EqPreset eq) {
  auto& g = Display::canvas();
  EqBands b = eqBands(eq);
  int v[3] = { b.low, b.peak, b.high };
  for (int i = 0; i < 3; i++) {
    int cx = 170 + i * 34;
    int mag = v[i] < 0 ? -v[i] : v[i];
    int w2 = mag > 6 ? 12 : mag * 12 / 6;
    g.drawFastHLine(cx - 12, y + 31, 24, cTrackBd);
    if (v[i] >= 0) g.fillRect(cx, y + 25, w2, 6, cBlue);
    else           g.fillRect(cx - w2, y + 31, w2, 6, cYellow);
  }
}

void settingsRows(const SettingsData& s, int selected) {
  const int ITEMS = 8, VISIBLE = 4;
  const int y0 = 34, rh = 44;
  int top = selected - (VISIBLE - 1);
  if (top > ITEMS - VISIBLE) top = ITEMS - VISIBLE;
  if (top < 0) top = 0;

  // 清除旧选中阴影；整个过程只发生在画布里。
  Display::canvas().fillRect(8, 34, 310, 183, cPaper);

  for (int r = 0; r < VISIBLE; r++) {
    int i = top + r;
    if (i >= ITEMS) break;
    int y = y0 + r * rh;
    switch (i) {
      case 0:
        settingsRow(y, "亮度", String(s.brightness) + "%", selected == i, s.brightness);
        break;
      case 1:
        settingsRow(y, "旋转方向",
                    (s.rotation == 1) ? "横屏 0°" : "横屏 180°", selected == i, -1);
        break;
      case 2:
        settingsRow(y, "均衡器", eqLabelCN(s.eq), selected == i, -1);
        settingsBands(y, s.eq);
        break;
      case 3:
        settingsRow(y, "刷新列表", "单击OK", selected == i, -1);
        break;
      case 4:
        settingsRow(y, "蓝牙广播", s.bleEnabled ? "开" : "关", selected == i, -1);
        break;
      case 5:
        settingsRow(y, "WiFi", s.wifiEnabled ? "开" : "关", selected == i, -1);
        break;
      case 6:
        settingsRow(y, "热点 AP", s.apEnabled ? "开" : "关", selected == i, -1);
        break;
      case 7:
        settingsRow(y, "显示绑定码", "单击查看", selected == i, -1);
        break;
      default:
        break;
    }
  }

  /* 右侧滚动条: 4 行窗口在 7 项中的位置 */
  auto& g = Display::canvas();
  const int sx = 314, sy = y0, sh = rh * VISIBLE, sw = 4;
  int thumbH = sh * VISIBLE / ITEMS;
  int thumbY = sy + (sh - thumbH) * top / (ITEMS - VISIBLE);
  g.fillRoundRect(sx, sy, sw, sh, 2, cTrack);
  g.fillRoundRect(sx, thumbY, sw, thumbH, 2, cCoral);
}

} // namespace

namespace Ui {

void begin() { palette(); }

/* 顶栏通知位: 卡片插入状态。任意界面都能即时更新 (不依赖整屏重绘) */
void setHall(bool detected) {
  if (detected == s_hallNow) return;
  s_hallNow  = detected;
  s_lastHall = -1;                     // 强制重绘图标
  drawHallIcon(detected);
  Serial.printf("[HALL] %s\n", detected ? "card in" : "card out");
}

void boot(const char* line1, const char* line2) {
  palette();
  auto& g = Display::canvas();
  g.fillScreen(cPaper);
  drawTopBarBase();
  drawLogo(160, 84, 12);
  g.setFont(Fonts::title());
  g.setTextColor(cInk, cPaper);
  g.drawCenterString(line1, 160, 108);
  if (line2) {
    g.setFont(Fonts::body());
    g.setTextColor(cInkSoft, cPaper);
    g.drawCenterString(line2, 160, 150);
  }
}

void drawStatic() {
  palette();
  auto& g = Display::canvas();
  g.fillScreen(cPaper);
  drawTopBarBase();
  drawCover();
  drawCategory();
  drawTransport();
  s_lastIdx = -999; s_lastCnt = -1; s_lastVol = -1;
  s_lastCur = 0xFFFFFFFF; s_lastDur = 0xFFFFFFFF;
  s_lastState = -1; s_lastHall = -1;
}

void update(const PlayerStatus& st, bool hallDetected) {
  palette();

  /* 先取旧状态, drawCount 会更新 idx/cnt 缓存 */
  int state = (st.playing ? 2 : 0) | (st.paused ? 1 : 0) | (st.hasTrack ? 4 : 0);
  bool trackChanged = (st.index != s_lastIdx) || (st.count != s_lastCnt)
                   || ((state ^ s_lastState) & 4);
  drawCount(st);

  if (trackChanged) drawTitleSub(st);
  scrollTitle();                              // 曲名跑马灯推进
  if (state != s_lastState) {
    s_lastState = state;
    drawStatus(st);
    drawPlayButton(st);
  }
  if (st.curSec != s_lastCur || st.durSec != s_lastDur) {
    s_lastCur = st.curSec; s_lastDur = st.durSec;
    drawTimes(st);
    drawProgress(st);
  }
  if (st.volume != s_lastVol) {
    s_lastVol = st.volume;
    drawVolume(st);
  }
  /* 编码/比特率: 解码器解到第一帧后才有值, 内容变化时单独补绘 */
  if (subText(st) != s_subLast) {
    drawSubLine(st);
  }
  setHall(hallDetected);                      // 顶栏通知位: 状态变化时显隐图标
  if (s_hallLayout != s_hallNow) {            // 音量文字右缘随之让位 / 收回
    s_hallLayout = s_hallNow;
    drawVolume(st);
  }
}

void drawSettings(const SettingsData& s, int selected) {
  palette();
  auto& g = Display::canvas();
  g.fillScreen(cPaper);
  drawTopBarBase();

  settingsRows(s, selected);

  g.setFont(Fonts::body());
  g.setTextColor(cInkSoft, cPaper);
  g.drawCenterString("上下选择  旋钮调整  OK执行", 160, 220);
}

void settingsMessage(const char* message) {
  auto& g = Display::canvas();
  g.fillRect(0, 218, SCR_W, 22, cPaper);
  g.setFont(Fonts::body());
  g.setTextColor(cInkSoft, cPaper);
  g.drawCenterString(fit(String(message), 304).c_str(), 160, 220);
}

/* 家长锁定页: 所有输入由 main 屏蔽, 只显示原因与解锁提示 */
void drawLock(const char* title, const char* line1, const char* line2) {
  palette();
  auto& g = Display::canvas();
  g.fillScreen(cPaper);
  drawTopBarBase();

  const int cx = 160, y = 48;
  g.fillRoundRect(cx - 36, y, 72, 34, 12, cCoralDk);
  g.fillTriangle(cx - 34, y + 30, cx + 34, y + 30, cx, y + 66, cCoralDk);
  g.fillRoundRect(cx - 32, y + 3, 64, 28, 10, cCoral);
  g.fillTriangle(cx - 28, y + 28, cx + 28, y + 28, cx, y + 58, cCoral);
  g.fillCircle(cx, y + 20, 13, cYellow);
  g.setFont(Fonts::body());
  g.setTextColor(cCoral, cYellow);
  g.drawCenterString("护", cx, y + 12);

  g.setFont(Fonts::title());
  g.setTextColor(cInk, cPaper);
  g.drawCenterString(fit(String(title ? title : "家长控制中"), 296).c_str(), 160, 124);

  g.setFont(Fonts::body());
  g.setTextColor(cInkSoft, cPaper);
  if (line1) g.drawCenterString(fit(String(line1), 300).c_str(), 160, 158);
  if (line2) g.drawCenterString(fit(String(line2), 300).c_str(), 160, 182);

  g.setFont(Fonts::small());
  g.setTextColor(cInkSoft, cPaper);
  g.drawCenterString("家长可在小程序中解锁", 160, 214);
}

/* 首次绑定页: 显示 6 位绑定码, 供家长在小程序输入 */
void drawBindCode(const char* code) {
  palette();
  auto& g = Display::canvas();
  g.fillScreen(cPaper);
  drawTopBarBase();
  g.setFont(Fonts::title());
  g.setTextColor(cInk, cPaper);
  g.drawCenterString("故事机配对", 160, 58);
  g.setFont(Fonts::body());
  g.setTextColor(cInkSoft, cPaper);
  g.drawCenterString("请在小程序中输入绑定码", 160, 92);

  g.fillRoundRect(96, 118, 128, 54, 12, TFT_WHITE);
  g.drawRoundRect(96, 118, 128, 54, 12, cTrackBd);
  g.setFont(Fonts::title());
  g.setTextColor(cCoral, TFT_WHITE);
  char spaced[16];
  snprintf(spaced, sizeof(spaced), "%c %c %c %c %c %c",
           code ? code[0] : '0', code ? code[1] : '0', code ? code[2] : '0',
           code ? code[3] : '0', code ? code[4] : '0', code ? code[5] : '0');
  g.drawCenterString(spaced, 160, 132);

  g.setFont(Fonts::small());
  g.setTextColor(cInkSoft, cPaper);
  g.drawCenterString("首次配对时输入 · 长按SET返回", 160, 196);
}

/* 家长未开放任何内容 */
void drawNoContent() {
  palette();
  auto& g = Display::canvas();
  g.fillScreen(cPaper);
  drawTopBarBase();
  g.setFont(Fonts::title());
  g.setTextColor(cInk, cPaper);
  g.drawCenterString("家长未开放内容", 160, 92);
  g.setFont(Fonts::body());
  g.setTextColor(cInkSoft, cPaper);
  g.drawCenterString("请在家长控制中开启故事或音乐", 160, 132);
  g.drawCenterString("保存后设备会自动刷新列表", 160, 160);
}

void refreshSettings(const SettingsData& s, int selected) {
  palette();
  settingsRows(s, selected);
}

/* ---------------- 音乐分类列表 ----------------
   顶栏 + 4 行分类卡片 (每行 42px); 置顶第 0 项为"全部歌曲"(递归收录 /music 全部 mp3),
   其余为 /music 的一级子文件夹 (分类名取自 Storage::catName) */

namespace {
  const int MUSIC_Y0 = 36, MUSIC_RH = 42, MUSIC_VIS = 4;

  /* ♪ 音符图标 (全部歌曲行): 约 20x20 @ (x,y) */
  void iconNote(int x, int y, uint16_t c) {
    auto& g = Display::canvas();
    g.fillCircle(x + 6, y + 14, 5, c);
    g.drawCircle(x + 6, y + 14, 5, cInk);
    g.fillRect(x + 10, y + 2, 3, 14, cInk);
    g.fillRect(x + 13, y + 2, 7, 4, cInk);
  }

  /* 文件夹图标: 约 22x18 @ (x,y) */
  void iconFolder(int x, int y, uint16_t c) {
    auto& g = Display::canvas();
    g.fillRect(x, y + 2, 10, 5, c);
    g.drawRect(x, y + 2, 10, 5, cInk);
    g.fillRoundRect(x, y + 5, 22, 15, 3, c);
    g.drawRoundRect(x, y + 5, 22, 15, 3, cInk);
  }

  /* 分类行卡片: 白底 + 描边; 选中: 黄底 + 墨色描边 + 3px 硬阴影 */
  void musicRow(int y, const char* name, int i, bool selected) {
    auto& g = Display::canvas();
    if (selected) g.fillRoundRect(13, y + 3, 300, 38, 6, cInk);
    g.fillRoundRect(10, y, 300, 38, 6, selected ? cInk : TFT_WHITE);
    g.fillRoundRect(12, y + 2, 296, 34, 5, selected ? cYellow : TFT_WHITE);
    if (!selected) g.drawRoundRect(10, y, 300, 38, 6, cTrackBd);

    uint16_t tint = selected ? cCoral : cAqua;
    if (i == 0) iconNote(24, y + 8, tint);
    else        iconFolder(24, y + 10, tint);

    uint16_t bg = selected ? cYellow : TFT_WHITE;
    g.setFont(Fonts::title());
    g.setTextColor(cInk, bg);
    g.drawString(fit(String(name), 206).c_str(), 54, y + 9);

    char b[16];
    snprintf(b, sizeof(b), "%d首", Storage::viewCatTrackCount(i));
    g.setFont(Fonts::small());
    g.setTextColor(selected ? cInk : cInkSoft, bg);
    g.drawRightString(b, 296, y + 14);
  }

  int musicTop(int selected, int count) {
    int top = (selected >= MUSIC_VIS) ? selected - MUSIC_VIS + 1 : 0;
    if (top > count - MUSIC_VIS) top = count - MUSIC_VIS;
    if (top < 0) top = 0;
    return top;
  }

  /* 只重绘一行槽位: 先清掉本行的卡片 + 3px 硬阴影, 再按当前状态重画该行。
     不清整屏, 所以没有"先刷底色再画回来"的中间态 => 不闪动 */
  void musicRepaintRow(int r, int top, int count, int selected) {
    auto& g = Display::canvas();
    int y = MUSIC_Y0 + r * MUSIC_RH;
    g.fillRect(8, y, 306, MUSIC_RH, cPaper);
    int i = top + r;
    if (i < count) musicRow(y, Storage::viewCatName(i), i, i == selected);
  }

  void musicRows(int count, int selected) {
    int top = musicTop(selected, count);
    for (int r = 0; r < MUSIC_VIS; r++) musicRepaintRow(r, top, count, selected);
  }

  /* 上次绘制的窗口状态; top < 0 表示还没有整屏底子, 需要整屏重画 */
  int s_mlTop = -1, s_mlCount = -1, s_mlSel = -1;
}

void drawMusicList(int count, int selected) {
  palette();
  auto& g = Display::canvas();
  g.fillScreen(cPaper);
  drawTopBarBase();
  musicRows(count, selected);

  g.setFont(Fonts::body());
  g.setTextColor(cInkSoft, cPaper);
  g.drawCenterString("OK播放 双击设置 长按SET切换", 160, 212);

  s_mlTop   = musicTop(selected, count);
  s_mlCount = count;
  s_mlSel   = selected;
}

/* 选择变化时的增量刷新: 只重绘发生变化的行 (不清屏) */
void updateMusicList(int count, int selected) {
  palette();
  if (s_mlTop < 0 || s_mlCount != count) { drawMusicList(count, selected); return; }

  int top = musicTop(selected, count);
  if (top != s_mlTop) {
    /* 窗口滚动: 可见的 4 行内容全变了, 逐行重绘 */
    for (int r = 0; r < MUSIC_VIS; r++) musicRepaintRow(r, top, count, selected);
  } else {
    /* 仅选中行移动: 旧选中行变回未选中, 新选中行变选中 */
    musicRepaintRow(s_mlSel - top, top, count, selected);
    musicRepaintRow(selected - top, top, count, selected);
  }
  s_mlTop = top; s_mlCount = count; s_mlSel = selected;
}

/* ---------------- 当前歌曲列表 ----------------
   顶栏 + 4 行曲目卡片 (每行 42px); 正在播放的曲目以珊瑚色 ♪ 图标 + 珊瑚色标题 + "播放中" 标记 */

namespace {
  /* 去掉 .mp3 后缀 */
  String stripExt(const String& n) {
    int k = n.lastIndexOf('.');
    return (k > 0) ? n.substring(0, k) : n;
  }

  /* 曲目行卡片: 白底 + 描边; 选中: 黄底 + 墨色描边 + 3px 硬阴影 */
  void trackRow(int y, const char* name, bool playing, bool selected) {
    auto& g = Display::canvas();
    if (selected) g.fillRoundRect(13, y + 3, 300, 38, 6, cInk);
    g.fillRoundRect(10, y, 300, 38, 6, selected ? cInk : TFT_WHITE);
    g.fillRoundRect(12, y + 2, 296, 34, 5, selected ? cYellow : TFT_WHITE);
    if (!selected) g.drawRoundRect(10, y, 300, 38, 6, cTrackBd);

    uint16_t bg = selected ? cYellow : TFT_WHITE;
    iconNote(24, y + 8, playing ? cCoral : cAqua);          // 正在播放: 珊瑚色 ♪ 高亮

    g.setFont(Fonts::title());
    g.setTextColor(playing ? cCoral : cInk, bg);
    g.drawString(fit(stripExt(String(name)), 200).c_str(), 54, y + 9);

    if (playing) {
      g.setFont(Fonts::small());
      g.setTextColor(cCoral, bg);
      g.drawRightString("播放中", 296, y + 14);
    }
  }

  int trackTop(int selected, int count) {
    int top = (selected >= MUSIC_VIS) ? selected - MUSIC_VIS + 1 : 0;
    if (top > count - MUSIC_VIS) top = count - MUSIC_VIS;
    if (top < 0) top = 0;
    return top;
  }

  /* 只重绘一行槽位 (含清行), 说明见 musicRepaintRow */
  void trackRepaintRow(int r, int top, int count, int selected, int playing) {
    auto& g = Display::canvas();
    int y = MUSIC_Y0 + r * MUSIC_RH;
    g.fillRect(8, y, 306, MUSIC_RH, cPaper);
    int i = top + r;
    if (i < count) trackRow(y, Storage::name(i), i == playing, i == selected);
  }

  void trackRows(int count, int selected, int playing) {
    int top = trackTop(selected, count);
    for (int r = 0; r < MUSIC_VIS; r++) trackRepaintRow(r, top, count, selected, playing);
  }

  int s_tlTop = -1, s_tlCount = -1, s_tlSel = -1, s_tlPlay = -1;
}

void drawTrackList(int count, int selected, int playing) {
  palette();
  auto& g = Display::canvas();
  g.fillScreen(cPaper);
  drawTopBarBase();
  trackRows(count, selected, playing);

  g.setFont(Fonts::body());
  g.setTextColor(cInkSoft, cPaper);
  g.drawCenterString("OK播放 双击设置 长按返回", 160, 212);

  s_tlTop = trackTop(selected, count); s_tlCount = count;
  s_tlSel = selected;                  s_tlPlay  = playing;
}

/* 增量刷新: 选中行 / "播放中"高亮 各自变化时只重绘对应行 (不清屏) */
void updateTrackList(int count, int selected, int playing) {
  palette();
  if (s_tlTop < 0 || s_tlCount != count) { drawTrackList(count, selected, playing); return; }

  int top = trackTop(selected, count);
  if (top != s_tlTop) {
    for (int r = 0; r < MUSIC_VIS; r++) trackRepaintRow(r, top, count, selected, playing);
  } else {
    int idx[4] = { s_tlSel, s_tlPlay, selected, playing };
    for (int k = 0; k < 4; k++) {
      bool duplicate = false;
      for (int j = 0; j < k; ++j) duplicate |= idx[j] == idx[k];
      if (duplicate) continue;
      int r = idx[k] - top;
      if (r >= 0 && r < MUSIC_VIS) trackRepaintRow(r, top, count, selected, playing);
    }
  }
  s_tlTop = top; s_tlCount = count; s_tlSel = selected; s_tlPlay = playing;
}

/* ---------------- 故事模式 ----------------
   列表: 顶栏 + 4 行故事卡片 (每行 42px)
   播放: 全屏插画 (320x240 JPEG, 画面贴顶) + 底部迷你控制条
         (黄色顶沿 + 圆形播放键 + 故事名 + 进度条 + 页码 + 时间/音量) */

namespace {
  const int STORY_Y0  = 36, STORY_RH = 42, STORY_VIS = 4;

  /* 故事行卡片: 白底 + 描边; 选中: 黄底 + 墨色描边 + 3px 硬阴影 */
  void storyRow(int y, const char* name, bool selected) {
    auto& g = Display::canvas();
    if (selected) g.fillRoundRect(13, y + 3, 300, 38, 6, cInk);
    g.fillRoundRect(10, y, 300, 38, 6, selected ? cInk : TFT_WHITE);
    g.fillRoundRect(12, y + 2, 296, 34, 5, selected ? cYellow : TFT_WHITE);
    if (!selected) g.drawRoundRect(10, y, 300, 38, 6, cTrackBd);

    /* 小书图标 */
    g.fillRoundRect(22, y + 8, 20, 22, 3, selected ? cCoral : cAqua);
    g.drawRoundRect(22, y + 8, 20, 22, 3, cInk);
    g.fillRect(25, y + 8, 3, 22, cInk);

    uint16_t bg = selected ? cYellow : TFT_WHITE;
    g.setFont(Fonts::title());
    g.setTextColor(cInk, bg);
    g.drawString(fit(String(name), 248).c_str(), 50, y + 9);
  }

  int storyTop(int selected, int count) {
    int top = (selected >= STORY_VIS) ? selected - STORY_VIS + 1 : 0;
    if (top > count - STORY_VIS) top = count - STORY_VIS;
    if (top < 0) top = 0;
    return top;
  }

  /* 只重绘一行槽位 (含清行), 说明见 musicRepaintRow */
  void storyRepaintRow(int r, int top, int count, int selected) {
    auto& g = Display::canvas();
    int y = STORY_Y0 + r * STORY_RH;
    g.fillRect(8, y, 306, STORY_RH, cPaper);
    int i = top + r;
    if (i < count) storyRow(y, Story::viewTitle(i), i == selected);
  }

  void storyRows(int count, int selected) {
    int top = storyTop(selected, count);
    for (int r = 0; r < STORY_VIS; r++) storyRepaintRow(r, top, count, selected);
  }

  int s_slTop = -1, s_slCount = -1, s_slSel = -1;
}

void drawStoryList(int count, int selected) {
  palette();
  auto& g = Display::canvas();
  g.fillScreen(cPaper);
  drawTopBarBase();
  storyRows(count, selected);

  g.setFont(Fonts::body());
  g.setTextColor(cInkSoft, cPaper);
  g.drawCenterString("OK播放 双击设置 长按SET切换", 160, 212);

  s_slTop   = storyTop(selected, count);
  s_slCount = count;
  s_slSel   = selected;
}

/* 选择变化时的增量刷新: 只重绘发生变化的行 (不清屏) */
void updateStoryList(int count, int selected) {
  palette();
  if (s_slTop < 0 || s_slCount != count) { drawStoryList(count, selected); return; }

  int top = storyTop(selected, count);
  if (top != s_slTop) {
    for (int r = 0; r < STORY_VIS; r++) storyRepaintRow(r, top, count, selected);
  } else {
    storyRepaintRow(s_slSel - top, top, count, selected);
    storyRepaintRow(selected - top, top, count, selected);
  }
  s_slTop = top; s_slCount = count; s_slSel = selected;
}

namespace {
/* 加载页旋转圆环几何 (静态层与动画层共用) */
const int SL_CX = 160, SL_CY = 102;
}

/* 8 点旋转圆环: 珊瑚作头, 黄/青为尾, 其余灰点
   半径固定为 5, 每帧只把新颜色原位覆盖到旧圆点上 => 无需清屏, 完全没有闪动 */
void drawStoryLoadingSpin(int step) {
  auto& g = Display::canvas();
  static const int8_t DX[8] = { 30, 21,  0, -21, -30, -21,  0, 21 };
  static const int8_t DY[8] = {  0, 21, 30,  21,   0, -21, -30, -21 };
  for (int i = 0; i < 8; i++) {
    int k = ((step - i) % 8 + 8) % 8;
    uint16_t c = (k == 0) ? cCoral : (k == 1) ? cYellow : (k == 2) ? cAqua : cTrackBd;
    g.fillCircle(SL_CX + DX[i], SL_CY + DY[i], 5, c);
  }
}

void drawStoryLoadingBegin(const char* title) {
  palette();
  auto& g = Display::canvas();
  g.fillScreen(cPaper);
  drawTopBarBase();
  drawStoryLoadingSpin(0);

  /* 环心小书图标 */
  g.fillRoundRect(SL_CX - 15, SL_CY - 17, 30, 34, 4, cCoral);
  g.drawRoundRect(SL_CX - 15, SL_CY - 17, 30, 34, 4, cInk);
  g.fillRect(SL_CX - 9, SL_CY - 17, 4, 34, cInk);      // 书脊
  g.fillRect(SL_CX - 3, SL_CY - 8, 12, 3, cYellow);
  g.fillRect(SL_CX - 3, SL_CY - 1, 12, 3, cYellow);

  /* 故事名 + 提示文字 */
  g.setFont(Fonts::story());
  g.setTextColor(cInk, cPaper);
  g.drawCenterString(fit(String(title ? title : ""), 288).c_str(), 160, 168);

  g.setFont(Fonts::body());
  g.setTextColor(cInkSoft, cPaper);
  g.drawCenterString("正在打开故事…", 160, 194);
}

void drawStoryFrame(const char* imgPath) {
  palette();
  auto& g = Display::canvas();
  if (!imgPath) return;
  if (!g.drawJpgFile(SD_MMC, imgPath, 0, 0, SCR_W, SCR_H)) {
    g.fillScreen(TFT_BLACK);
    g.setFont(Fonts::body());
    g.setTextColor(TFT_WHITE, TFT_BLACK);
    g.drawCenterString("插画读取失败", 160, 110);
  }
}

namespace {
/* 迷你控制条几何 (320x240): 高度 38px, 尽量少遮挡插画 */
const int SB_Y      = 202, SB_H      = 38;                 // 控制条区域
const int SB_BTN_X  =   6, SB_BTN_Y  = 207, SB_BTN_W = 26, SB_BTN_H = 26;
const int SB_TTL_X  =  38, SB_TTL_Y  = 206, SB_TTL_W = 200, SB_TTL_H = 21;
const int SB_PG_X   = 268, SB_PG_Y   = 207, SB_PG_W  = 44; // 页码 "NN/NN" (右对齐 312)
const int SB_PR_X   =  38, SB_PR_Y   = 229, SB_PR_W  = 200, SB_PR_H = 8;
const int SB_TXT_X  = 240, SB_TXT_Y  = 228, SB_TXT_W  = 72; // 时间/音量槽 (右对齐 312)

/* 增量刷新缓存: 逐区域只重绘变化部分, 避免每秒整条重铺造成的闪动 */
bool     s_sbInit   = false;                               // 静态层(黑底/顶沿/按钮框/进度槽)是否已铺好
uint32_t s_sbCur    = 0xFFFFFFFF, s_sbDur = 0xFFFFFFFF;
int      s_sbPanel  = -1, s_sbTotal = -1, s_sbVol = -1, s_sbLastW = -1;
bool     s_sbPaused = false;
uint32_t s_sbVolUntil = 0;                                 // 音量提示显示截止时刻
bool     s_sbVolShown = false;                             // 文本槽当前是否显示音量

/* 故事名跑马灯 */
String   s_stName;                                         // 当前故事名
int16_t  s_stW = 0, s_stScroll = 0;
bool     s_stDirty = true;
uint32_t s_stTick = 0;

/* 播放按钮图标: 播放中显示暂停双竖条, 暂停时显示播放三角 */
void storyBtnIcon(bool paused) {
  auto& g = Display::canvas();
  int cx = SB_BTN_X + SB_BTN_W / 2, cy = SB_BTN_Y + SB_BTN_H / 2;
  g.fillRoundRect(SB_BTN_X + 2, SB_BTN_Y + 2, SB_BTN_W - 4, SB_BTN_H - 4, 6, cYellow);
  if (paused) {
    g.fillTriangle(cx - 4, cy - 5, cx - 4, cy + 5, cx + 5, cy, cInk);
  } else {
    g.fillRect(cx - 4, cy - 5, 3, 10, cInk);
    g.fillRect(cx + 1, cy - 5, 3, 10, cInk);
  }
}

/* 故事名行: 超长时按 s_stScroll 偏移绘制, 裁剪限制在标题区内 */
void storyTitleLine() {
  auto& g = Display::canvas();
  g.setFont(Fonts::story());
  g.setTextColor(TFT_WHITE, cInk);
  g.fillRect(SB_TTL_X, SB_TTL_Y, SB_TTL_W, SB_TTL_H, cInk);
  g.setClipRect(SB_TTL_X, SB_TTL_Y, SB_TTL_W, SB_TTL_H);
  g.drawString(s_stName.c_str(), SB_TTL_X - s_stScroll, SB_TTL_Y + 1);
  g.clearClipRect();
}
}

/* 整屏重绘插画后调用: 使控制条下次绘制时重新铺设静态层 */
void invalidateStoryBar() { s_sbInit = false; }

/* 开始播放某个故事时调用: 更新控制条上的故事名并重置跑马灯 */
void setStoryTitle(const char* title) {
  s_stName  = title ? title : "";
  s_stDirty = true;
}

void drawStoryBar(int panel, int total, uint32_t curSec, uint32_t durSec,
                  uint8_t volume, bool paused) {
  palette();
  auto& g = Display::canvas();
  char b[24];

  if (!s_sbInit) {                                   // 首次(或 invalidate 后): 铺静态层
    g.fillRect(0, SB_Y, SCR_W, SB_H, cInk);          // 墨色控制条整块铺满
    g.fillRect(0, SB_Y, SCR_W, 3, cYellow);          // 黄色顶沿
    g.fillRoundRect(SB_BTN_X - 2, SB_BTN_Y - 2, SB_BTN_W + 4, SB_BTN_H + 4, 7, cYellowLt);
    g.fillRoundRect(SB_BTN_X, SB_BTN_Y, SB_BTN_W, SB_BTN_H, 5, cYellow);
    g.fillRoundRect(SB_PR_X, SB_PR_Y, SB_PR_W, SB_PR_H, 2, cTrack);   // 进度槽
    g.drawRoundRect(SB_PR_X, SB_PR_Y, SB_PR_W, SB_PR_H, 2, cTrackBd);
    s_sbInit   = true;
    s_sbCur    = s_sbDur = 0xFFFFFFFF;
    s_sbPanel  = -1; s_sbTotal = -1; s_sbLastW = -1;
    s_sbVol    = (int)volume;                        // 首次不触发音量提示
    s_sbVolUntil = 0; s_sbVolShown = false;
    s_sbPaused = !paused;                            // 强制重绘按钮图标
    s_stDirty  = true;                               // 强制重绘故事名
    s_stTick   = millis();
  }

  /* 故事名：按经过时间滚动 */
  if (s_stDirty) {
    s_stDirty  = false;
    g.setFont(Fonts::story());
    s_stW      = g.textWidth(s_stName);
    s_stScroll = 0;
    s_stTick   = millis();
    storyTitleLine();
  } else if (s_stW > SB_TTL_W && s_stName.length()) {
    int offset = scrollOffset(s_stW, SB_TTL_W, s_stTick);
    if (offset != s_stScroll) {
      s_stScroll = offset;
      storyTitleLine();
    }
  }

  /* 页码 (每次换插画变化, 只清这一小块) */
  if (panel != s_sbPanel || total != s_sbTotal) {
    s_sbPanel = panel; s_sbTotal = total;
    snprintf(b, sizeof(b), "%02d/%02d", panel + 1, total);
    g.setFont(Fonts::small());
    g.setTextColor(cYellow, cInk);
    g.fillRect(SB_PG_X, SB_PG_Y, SB_PG_W, 14, cInk);
    g.drawRightString(b, 312, SB_PG_Y + 1);
  }

  /* 播放键图标 */
  if (paused != s_sbPaused) {
    s_sbPaused = paused;
    storyBtnIcon(paused);
  }

  /* 右下文本槽: 平时显示播放时间, 调节音量后 2s 内改显音量 */
  bool volChanged = ((int)volume != s_sbVol);
  if (volChanged) {
    s_sbVol = (int)volume;
    s_sbVolUntil = millis() + 2000;
  }
  if (millis() < s_sbVolUntil) {
    if (!s_sbVolShown || volChanged) {
      s_sbVolShown = true;
      g.setFont(Fonts::small());
      g.setTextColor(cTrack, cInk);
      g.fillRect(SB_TXT_X, SB_TXT_Y, SB_TXT_W, 12, cInk);
      snprintf(b, sizeof(b), "VOL %02u", (unsigned)volume);
      g.drawRightString(b, 312, SB_TXT_Y);
    }
  } else if (s_sbVolShown || curSec != s_sbCur || durSec != s_sbDur) {
    s_sbVolShown = false;
    s_sbCur = curSec; s_sbDur = durSec;
    g.setFont(Fonts::small());
    g.setTextColor(cTrack, cInk);
    g.fillRect(SB_TXT_X, SB_TXT_Y, SB_TXT_W, 12, cInk);
    snprintf(b, sizeof(b), "%s/%s", fmtSec(curSec).c_str(), fmtSec(durSec).c_str());
    g.drawRightString(b, 312, SB_TXT_Y);
  }

  /* 进度填充: 只补画新增段, 回退时重铺槽底 */
  int w = 0;
  if (durSec > 0) {
    w = (int)((uint64_t)(SB_PR_W - 2) * curSec / durSec);
    if (w > SB_PR_W - 2) w = SB_PR_W - 2;
  }
  if (w != s_sbLastW) {
    if (w < s_sbLastW) {
      g.fillRoundRect(SB_PR_X, SB_PR_Y, SB_PR_W, SB_PR_H, 2, cTrack);
      g.drawRoundRect(SB_PR_X, SB_PR_Y, SB_PR_W, SB_PR_H, 2, cTrackBd);
    }
    if (w > 1) {
      g.fillRect(SB_PR_X + 1, SB_PR_Y + 1, w, SB_PR_H - 2, cBlue);
      if (w > 4) g.fillRect(SB_PR_X + 1 + w - 4, SB_PR_Y + 1, 4, SB_PR_H - 2, cYellow);
    }
    s_sbLastW = w;
  }
}

} // namespace Ui
