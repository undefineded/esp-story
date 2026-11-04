#include "display.h"
#include "config.h"
#include "render_math.h"
#include <esp_heap_caps.h>
#include <cstring>
#include <atomic>
#include "esp_timer.h"
#ifndef ARDUINO
#include "esp_timer.h"
#include "esp_psram.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
static uint32_t millis() { return esp_timer_get_time() / 1000; }
static bool psramFound() { return esp_psram_get_size() != 0; }
#endif

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
      cfg.freq_write  = TFT_SPI_HZ;
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
static bool s_videoMode = false;
static std::atomic<uint32_t> s_submissions{0}, s_videoSubmissions{0}, s_bytes{0}, s_lastUs{0}, s_maxUs{0};
static constexpr int STRIP_H = 8;

/* 背光 PWM (经 Q1 SS8050 驱动, 占空比越高越亮)
   频率默认 25kHz: 5kHz 会落在可听频段, 让 IP5306 升压电路在某些亮度下啸叫。 */
static const int BL_CHANNEL = 7;
static const int BL_FREQ    = TFT_BL_FREQ;
static const int BL_RES     = TFT_BL_RES;
static bool      s_blReady  = false;
static uint8_t   s_blPercent = 100;

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
    s_transfer = static_cast<uint16_t*>(heap_caps_malloc(SCR_W * STRIP_H * 2 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
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
#ifdef ARDUINO
  Serial.printf("[UI] PSRAM canvas: %s\n", s_buffered ? "OK (300 KiB + 10 KiB staging)" : "unavailable, direct rendering");
  const uint32_t blActual = ledcSetup(BL_CHANNEL, BL_FREQ, BL_RES);
  ledcAttachPin(TFT_BL, BL_CHANNEL);
  Serial.printf("[UI] backlight PWM: %u Hz (asked %d), %d-bit\n",
                (unsigned)blActual, BL_FREQ, BL_RES);
#else
  ESP_LOGI("display", "PSRAM canvas: %s", s_buffered ? "ready" : "direct rendering fallback");
  ledc_timer_config_t timer{};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_8_BIT;
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = BL_FREQ;
  timer.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&timer));
  ledc_channel_config_t channel{};
  channel.gpio_num = TFT_BL;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = static_cast<ledc_channel_t>(BL_CHANNEL);
  channel.timer_sel = LEDC_TIMER_0;
  ESP_ERROR_CHECK(ledc_channel_config(&channel));
#endif
  s_blReady = true;
  setBrightness(100);
}

void setBrightness(uint8_t percent) {
  if (percent > 100) percent = 100;
  s_blPercent = percent;
  if (s_blReady) {
#ifdef ARDUINO
    ledcWrite(BL_CHANNEL, (uint32_t)percent * ((1 << BL_RES) - 1) / 100);
#else
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(BL_CHANNEL),
                                uint32_t(percent) * ((1 << BL_RES) - 1) / 100));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(BL_CHANNEL)));
#endif
  } else {                            // PWM 未就绪时退化为开关
#ifdef ARDUINO
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, percent > 0 ? HIGH : LOW);
#else
    ESP_ERROR_CHECK(gpio_set_direction(static_cast<gpio_num_t>(TFT_BL), GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(static_cast<gpio_num_t>(TFT_BL), percent > 0));
#endif
  }
}

/* 只换调光频率, 亮度保持 (重新配置定时器会把占空比清零, 这里补写一次) */
void setBacklightFreq(uint32_t hz) {
  if (!s_blReady || hz == 0) return;
#ifdef ARDUINO
  const uint32_t actual = ledcSetup(BL_CHANNEL, hz, BL_RES);
  ledcAttachPin(TFT_BL, BL_CHANNEL);
  ledcWrite(BL_CHANNEL, (uint32_t)s_blPercent * ((1 << BL_RES) - 1) / 100);
  Serial.printf("[UI] backlight PWM -> %u Hz (asked %u)\n", (unsigned)actual, (unsigned)hz);
#else
  ESP_ERROR_CHECK(ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, hz));
  ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(BL_CHANNEL),
                                uint32_t(s_blPercent) * ((1 << BL_RES) - 1) / 100));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(BL_CHANNEL)));
  ESP_LOGI("display", "backlight PWM -> %lu Hz (asked %lu)",
           static_cast<unsigned long>(hz), static_cast<unsigned long>(hz));
#endif
}

lgfx::LGFXBase& canvas() {
  s_dirty = true;
  return s_buffered ? static_cast<lgfx::LGFXBase&>(s_canvas) : static_cast<lgfx::LGFXBase&>(s_lcd);
}

void present(bool force) {
  if (!s_buffered || !s_dirty) return;
  uint32_t now = millis();
  // Video is paced by the UI's microsecond frame deadline. A second millis
  // throttle can skip an entire frame when drawing time varies slightly.
  if (!force && !s_videoMode && now - s_presentAt < 33) return;
  s_presentAt = now;
  const auto started = esp_timer_get_time();
  uint32_t bytes = 0;
  unsigned staging = 0;
  const auto* pixels = static_cast<const uint16_t*>(s_canvas.getBuffer());
  // Sprite RGB565 storage is already byte-swapped. Typed pixels preserve this
  // representation without relying on the LCD's mutable setSwapBytes setting.
  s_lcd.startWrite();
  for (int y = 0; y < SCR_H; y += STRIP_H) {
    const auto span = RenderMath::changedSpan(pixels + y * SCR_W,
        s_previous + y * SCR_W, SCR_W, STRIP_H, s_invalid || s_videoMode);
    const int left = span.left, right = span.right;
    if (right < left) continue;
    int width = right - left + 1;
    auto* transfer = s_transfer + staging * SCR_W * STRIP_H;
    for (int row = 0; row < STRIP_H; ++row) {
      int offset = (y + row) * SCR_W + left;
      memcpy(transfer + row * width, pixels + offset, width * 2);
      if (!s_videoMode) memcpy(s_previous + offset, pixels + offset, width * 2);
    }
    // Prepare the alternate buffer while the previous transfer is in flight.
    s_lcd.waitDMA();
    s_lcd.pushImageDMA(left, y, width, STRIP_H,
                    reinterpret_cast<const lgfx::swap565_t*>(transfer));
    staging ^= 1;
    bytes += width * STRIP_H * 2;
  }
  s_lcd.waitDMA();
  s_lcd.endWrite();
  s_invalid = s_videoMode; // Previous-frame mirror is stale after video mode.
  s_dirty = false;
  if (bytes) {
    ++s_submissions;
    if (s_videoMode) ++s_videoSubmissions;
    s_bytes += bytes;
    const auto elapsed = static_cast<uint32_t>(esp_timer_get_time() - started);
    s_lastUs = elapsed;
    if (elapsed > s_maxUs.load()) s_maxUs = elapsed;
  }
}

void setVideoMode(bool enabled) {
  if (s_videoMode != enabled) s_invalid = true;
  s_videoMode = enabled;
}
Stats stats() { return {s_submissions.load(), s_videoSubmissions.load(), s_bytes.load(), s_lastUs.load(), s_maxUs.load()}; }

void setRotation(uint8_t r) {
  lcd().setRotation(r);
  s_invalid = s_dirty = true;
}

}
