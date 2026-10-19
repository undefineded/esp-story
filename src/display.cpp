#include "display.h"
#include "config.h"
#include "render_math.h"
#include <esp_heap_caps.h>
#include <cstring>

/* ST7789 面板 + SPI 总线配置 (引脚见 config.h) */
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;      // ESP32-S3 通用 SPI
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = true;           // 只写不读
      cfg.use_lock    = true;
      cfg.pin_sclk    = TFT_SCK;
      cfg.pin_mosi    = TFT_MOSI;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = TFT_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs        = TFT_CS;
      cfg.pin_rst       = TFT_RST;
      cfg.pin_busy      = -1;
      cfg.panel_width   = 240;
      cfg.panel_height  = 320;
      cfg.offset_x      = 0;
      cfg.offset_y      = 0;
      cfg.offset_rotation = 0;
      cfg.readable      = false;
      cfg.invert        = true;         // 多数 ST7789 IPS 需反显
      cfg.rgb_order     = false;
      cfg.dlen_16bit    = false;
      cfg.bus_shared    = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

static LGFX s_lcd;
static lgfx::LGFX_Sprite s_canvas;
static uint16_t* s_previous = nullptr;
static uint16_t* s_transfer = nullptr;
static bool s_buffered = false, s_invalid = true, s_dirty = false;
static uint32_t s_presentAt = 0;
static constexpr int STRIP_H = 8;

/* 背光 PWM (经 Q1 SS8050 驱动, 占空比越高越亮) */
static const int BL_CHANNEL = 7;
static const int BL_FREQ    = 5000;
static const int BL_RES     = 8;
static bool      s_blReady  = false;

lgfx::LGFX_Device& lcd() { return s_lcd; }

namespace Display {

void begin() {
  auto& g = lcd();
  g.init();
  g.setRotation(3);                   // 默认横屏 180°
  g.fillScreen(TFT_BLACK);

  if (psramFound()) {
    s_canvas.setColorDepth(16);
    s_canvas.setPsram(true);
    s_previous = static_cast<uint16_t*>(heap_caps_malloc(SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_transfer = static_cast<uint16_t*>(heap_caps_malloc(SCR_W * STRIP_H * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    s_buffered = s_previous && s_transfer && s_canvas.createSprite(SCR_W, SCR_H);
  }
  if (s_buffered) {
    s_canvas.fillScreen(TFT_BLACK);
  } else {
    s_canvas.deleteSprite();
    heap_caps_free(s_previous);
    heap_caps_free(s_transfer);
    s_previous = s_transfer = nullptr;
  }
  Serial.printf("[UI] PSRAM canvas: %s\n", s_buffered ? "OK (300 KiB + 5 KiB staging)" : "unavailable, direct rendering");

  ledcSetup(BL_CHANNEL, BL_FREQ, BL_RES);
  ledcAttachPin(TFT_BL, BL_CHANNEL);
  s_blReady = true;
  setBrightness(100);
}

void setBrightness(uint8_t percent) {
  if (percent > 100) percent = 100;
  if (s_blReady) {
    ledcWrite(BL_CHANNEL, (uint32_t)percent * ((1 << BL_RES) - 1) / 100);
  } else {                            // PWM 未就绪时退化为开关
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, percent > 0 ? HIGH : LOW);
  }
}

lgfx::LGFXBase& canvas() {
  s_dirty = true;
  return s_buffered ? static_cast<lgfx::LGFXBase&>(s_canvas) : static_cast<lgfx::LGFXBase&>(s_lcd);
}

void present(bool force) {
  if (!s_buffered || !s_dirty) return;
  uint32_t now = millis();
  if (!force && now - s_presentAt < 33) return;
  s_presentAt = now;
  const auto* pixels = static_cast<const uint16_t*>(s_canvas.getBuffer());
  // Sprite RGB565 storage is already byte-swapped. Typed pixels preserve this
  // representation without relying on the LCD's mutable setSwapBytes setting.
  s_lcd.startWrite();
  for (int y = 0; y < SCR_H; y += STRIP_H) {
    const auto span = RenderMath::changedSpan(pixels + y * SCR_W,
        s_previous + y * SCR_W, SCR_W, STRIP_H, s_invalid);
    const int left = span.left, right = span.right;
    if (right < left) continue;
    int width = right - left + 1;
    for (int row = 0; row < STRIP_H; ++row) {
      int offset = (y + row) * SCR_W + left;
      memcpy(s_transfer + row * width, pixels + offset, width * 2);
      memcpy(s_previous + offset, pixels + offset, width * 2);
    }
    s_lcd.pushImageDMA(left, y, width, STRIP_H,
                    reinterpret_cast<const lgfx::swap565_t*>(s_transfer));
    s_lcd.waitDMA();  // staging memory must not change while SPI owns it
  }
  s_lcd.endWrite();
  s_invalid = s_dirty = false;
}

void setRotation(uint8_t r) {
  lcd().setRotation(r);
  s_invalid = s_dirty = true;
}

}
