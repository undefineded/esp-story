#include "pcm_output.h"
#include <limits>
namespace Audio {
PcmOutput::~PcmOutput() { stop(); }
esp_err_t PcmOutput::begin(uint32_t sample_rate) {
    if (channel_) return ESP_ERR_INVALID_STATE;
    if (sample_rate < 8000 || sample_rate > 48000) return ESP_ERR_INVALID_ARG;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    channel.dma_desc_num = 4;
    channel.dma_frame_num = 240;
    auto result = i2s_new_channel(&channel, &channel_, nullptr);
    if (result != ESP_OK) return result;
    i2s_std_config_t config{};
    config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    config.gpio_cfg.bclk = GPIO_NUM_16;
    config.gpio_cfg.ws = GPIO_NUM_17;
    config.gpio_cfg.dout = GPIO_NUM_18;
    config.gpio_cfg.din = I2S_GPIO_UNUSED;
    result = i2s_channel_init_std_mode(channel_, &config);
    if (result == ESP_OK) result = i2s_channel_enable(channel_);
    if (result != ESP_OK) { i2s_del_channel(channel_); channel_ = nullptr; return result; }
    enabled_ = true;
    return ESP_OK;
}
esp_err_t PcmOutput::write(const int16_t* samples, std::size_t frames,
                         std::size_t& written, uint32_t timeout_ms) {
    written = 0;
    if (!enabled_) return ESP_ERR_INVALID_STATE;
    if ((!samples && frames) || frames > std::numeric_limits<std::size_t>::max() / 4)
        return ESP_ERR_INVALID_ARG;
    if (!frames) return ESP_OK;
    // Caller must handle partial writes, including progress returned on timeout.
    return i2s_channel_write(channel_, samples, frames * 4, &written, timeout_ms);
}
esp_err_t PcmOutput::stop() {
    if (!channel_) return ESP_OK;
    if (enabled_) {
        const auto result = i2s_channel_disable(channel_);
        if (result != ESP_OK) return result;
        enabled_ = false;
    }
    const auto result = i2s_del_channel(channel_);
    if (result == ESP_OK) channel_ = nullptr;
    return result;
}
}
