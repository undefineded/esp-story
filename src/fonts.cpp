/* 字体组件: 从 flash 加载 VLW 抗锯齿圆体 (幼圆), 失败时回退内置点阵字体
   字形数据由 tools/gen_vlw_font.ps1 生成, 经 tools/embed_fonts.py 嵌入固件 */
#include "fonts.h"
#include "display.h"
#include <LovyanGFX.hpp>
#include <string.h>

/* 由 tools/embed_fonts.py 用 objcopy 生成的符号 */
extern "C" {
  extern const uint8_t _binary_ui16_vlw_start[];
  extern const uint8_t _binary_ui16_vlw_end[];
  extern const uint8_t _binary_ui15_vlw_start[];
  extern const uint8_t _binary_ui15_vlw_end[];
  extern const uint8_t _binary_ui11_vlw_start[];
  extern const uint8_t _binary_ui11_vlw_end[];
  extern const uint8_t _binary_ui14_vlw_start[];
  extern const uint8_t _binary_ui14_vlw_end[];
}

namespace {

/* flash 中的字体数据包装: VLWfont 需要 seek/read 随机访问接口 */
class FlashFont : public lgfx::v1::DataWrapper {
public:
  FlashFont(const uint8_t* base, uint32_t size) : _base(base), _size(size) {}

  int read(uint8_t* buf, uint32_t len) override {
    if (_pos >= _size) return 0;
    uint32_t n = len;
    if (n > _size - _pos) n = _size - _pos;
    memcpy(buf, _base + _pos, n);
    _pos += n;
    return (int)n;
  }
  void   skip(int32_t offset) override { _pos += offset; }
  bool   seek(uint32_t offset) override { _pos = offset; return true; }
  void   close() override {}
  int32_t tell() override { return (int32_t)_pos; }

private:
  const uint8_t* _base;
  uint32_t _size;
  uint32_t _pos = 0;
};

lgfx::VLWfont s_title, s_body, s_small, s_story;
bool s_ok = false, s_storyOk = false;

/* 校验后加载: 防止字体未正确嵌入时读到野数据 */
bool loadOne(lgfx::VLWfont& f, const uint8_t* begin, const uint8_t* end) {
  uint32_t size = (uint32_t)(end - begin);
  if (size < 24 + 28) return false;
  uint32_t count = ((uint32_t)begin[0] << 24) | ((uint32_t)begin[1] << 16) |
                   ((uint32_t)begin[2] << 8) | (uint32_t)begin[3];
  if (count < 1 || count > 20000) return false;
  return f.loadFont(new FlashFont(begin, size));
}

} // namespace

namespace Fonts {

void begin() {
  bool a = loadOne(s_title, _binary_ui16_vlw_start, _binary_ui16_vlw_end);
  bool b = loadOne(s_body,  _binary_ui15_vlw_start, _binary_ui15_vlw_end);
  bool c = loadOne(s_small, _binary_ui11_vlw_start, _binary_ui11_vlw_end);
  bool d = loadOne(s_story, _binary_ui14_vlw_start, _binary_ui14_vlw_end);
  s_ok = a && b && c;
  s_storyOk = d;
  Serial.printf("[SYS] VLW fonts: 16px=%s 15px=%s 11px=%s 14px=%s\n",
                a ? "OK" : "FAIL", b ? "OK" : "FAIL", c ? "OK" : "FAIL", d ? "OK" : "FAIL");
}

bool ok() { return s_ok; }

const lgfx::IFont* title() { return s_ok ? (const lgfx::IFont*)&s_title : (const lgfx::IFont*)&lgfx::fonts::efontCN_24; }
const lgfx::IFont* story() { return (s_ok && s_storyOk) ? (const lgfx::IFont*)&s_story : (const lgfx::IFont*)&lgfx::fonts::efontCN_16; }
const lgfx::IFont* body()  { return s_ok ? (const lgfx::IFont*)&s_body  : (const lgfx::IFont*)&lgfx::fonts::efontCN_16; }
const lgfx::IFont* small() { return s_ok ? (const lgfx::IFont*)&s_small : (const lgfx::IFont*)&lgfx::fonts::Font0; }

} // namespace Fonts