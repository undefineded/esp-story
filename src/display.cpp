#include "display.h"
#include "config.h"

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

void setRotation(uint8_t r) { lcd().setRotation(r); }

}