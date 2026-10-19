#include "waveshare_duplex_audio.h"

#ifdef USE_ESP32

#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <esp_log.h>

namespace esphome {
namespace waveshare_duplex_audio {

static const char *const TAG = "waveshare_duplex_audio";

static const uint32_t DMA_DESC_NUM  = 6;
static const uint32_t DMA_FRAME_NUM = 240;

// ── AEC pre-buffer ring helpers ─────────────────────────────────────────────
//
// Called by speaker task at every TTS onset.
// Fills AEC_ADVANCE zeros into the ring starting at the current read pointer,
// then advances wr by AEC_ADVANCE.  Does NOT touch rd to avoid a race with
// the mic task which may be concurrently reading.
void WaveshareDuplexAudio::init_aec_ref() {
  uint32_t rd = aec_ref_rd_.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < AEC_ADVANCE; ++i)
    aec_ref_ring_[(rd + i) & (AEC_REF_SIZE - 1)] = 0;
  aec_ref_wr_.store(rd + AEC_ADVANCE, std::memory_order_release);
}

// Called by speaker task after volume scaling, BEFORE i2s_channel_write.
// Pushes left-channel samples into the ring; drops newest on overflow.
void WaveshareDuplexAudio::push_aec_ref(const int16_t *stereo, size_t frames) {
  // The ring holds VOICE_SAMPLE_RATE (16 kHz) samples while the speaker writes at
  // the bus rate, so decimate by averaging each group of `ratio` frames. With a
  // 16 kHz bus ratio is 1 and this is exactly the old per-sample copy.
  const uint32_t ratio = this->get_decimation_ratio();
  uint32_t wr = aec_ref_wr_.load(std::memory_order_relaxed);
  uint32_t rd = aec_ref_rd_.load(std::memory_order_acquire);
  for (size_t i = 0; i + ratio <= frames; i += ratio) {
    if ((wr - rd) >= AEC_REF_SIZE) break;  // ring full — drop newest
    int32_t acc = 0;
    for (uint32_t k = 0; k < ratio; ++k)
      acc += stereo[(i + k) * 2];  // left ch → mono
    aec_ref_ring_[wr & (AEC_REF_SIZE - 1)] = (int16_t) (acc / (int32_t) ratio);
    wr++;
  }
  aec_ref_wr_.store(wr, std::memory_order_release);
}

// Called by mic task once per chunk.  Returns zeros when ring is empty.
// offset: read from ring[rd+offset] instead of ring[rd]; rd still advances by n.
void WaveshareDuplexAudio::pop_ref_samples(int16_t *out, size_t n, uint32_t offset) {
  uint32_t rd = aec_ref_rd_.load(std::memory_order_relaxed);
  uint32_t wr = aec_ref_wr_.load(std::memory_order_acquire);
  uint32_t fill = wr - rd;  // Distance from rd to wr (handles uint32_t wraparound correctly)
  for (size_t i = 0; i < n; ++i) {
    uint32_t pos = offset + i;
    if (pos < fill) {
      out[i] = aec_ref_ring_[(rd + pos) & (AEC_REF_SIZE - 1)];
    } else {
      out[i] = 0;  // ring empty or pos exceeds fill → silence (NLMS step ≈ 0, weights frozen)
    }
  }
  aec_ref_rd_.store(rd + n, std::memory_order_release);
}

// ── Component lifecycle ─────────────────────────────────────────────────────

// The ES8311 (speaker) and ES7210 (mic + hardware echo reference) share one
// BCLK/WS pair, so both directions must run the SAME frame layout. The ES7210
// only exposes its hardware AEC reference (MIC3) in TDM 4-slot mode, which
// means 4 slots x 16 bit = 64 BCLK per WS frame. TX therefore also runs TDM;
// the ES8311 is an I2S slave and simply latches the first 16 bits after each
// WS edge, so slots 0 and 2 carry left/right for it while slots 1 and 3 stay
// silent.
//
// ESP-IDF pairs the two directions and forces the *later* initialised channel
// to be the clock slave; TX is initialised first so it stays the master and
// both directions see the 64 x fs clock.
static void fill_tdm_config(i2s_tdm_config_t *cfg, uint32_t sample_rate, gpio_num_t mclk,
                            gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
  *cfg = {
      .clk_cfg = {
          .sample_rate_hz  = sample_rate,
          .clk_src         = I2S_CLK_SRC_DEFAULT,
          .ext_clk_freq_hz = 0,
          .mclk_multiple   = I2S_MCLK_MULTIPLE_256,
          .bclk_div        = 8,
      },
      .slot_cfg = {
          .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
          .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
          .slot_mode      = I2S_SLOT_MODE_STEREO,
          .slot_mask      = (i2s_tdm_slot_mask_t) (I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                                                   I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
          .ws_width       = I2S_TDM_AUTO_WS_WIDTH,
          .ws_pol         = false,
          .bit_shift      = true,
          .left_align     = false,
          .big_endian     = false,
          .bit_order_lsb  = false,
          .skip_mask      = false,
          .total_slot     = I2S_TDM_AUTO_SLOT_NUM,
      },
      .gpio_cfg = {
          .mclk = mclk,
          .bclk = bclk,
          .ws   = ws,
          .dout = dout,
          .din  = din,
          .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
      },
  };
}

void WaveshareDuplexAudio::setup() {
  i2s_chan_config_t chan_cfg = {
      .id               = I2S_NUM_0,
      .role             = I2S_ROLE_MASTER,
      .dma_desc_num     = DMA_DESC_NUM,
      .dma_frame_num    = DMA_FRAME_NUM,
      .auto_clear_after_cb  = true,   // silence DMA descriptors on underflow
      .auto_clear_before_cb = false,
      .intr_priority    = 0,
  };
  esp_err_t err = i2s_new_channel(&chan_cfg, &this->tx_handle_, &this->rx_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // TX first: TDM 4-slot, ESP32 master. Frames are [L, -, R, -] — see above.
  i2s_tdm_config_t tx_cfg;
  fill_tdm_config(&tx_cfg, this->sample_rate_, this->mclk_pin_, this->bclk_pin_,
                  this->lrck_pin_, this->dout_pin_, I2S_GPIO_UNUSED);
  err = i2s_channel_init_tdm_mode(this->tx_handle_, &tx_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_init_tdm_mode (TX) failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // RX: TDM 4-slot, same frame → ESP-IDF makes it the paired clock slave.
  // The ES7210 outputs MIC1, MIC3(ref), MIC2, MIC4 in that slot order.
  i2s_tdm_config_t rx_cfg;
  fill_tdm_config(&rx_cfg, this->sample_rate_, this->mclk_pin_, this->bclk_pin_,
                  this->lrck_pin_, I2S_GPIO_UNUSED, this->din_pin_);
  err = i2s_channel_init_tdm_mode(this->rx_handle_, &rx_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_init_tdm_mode (RX) failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  err = i2s_channel_enable(this->tx_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_enable (TX) failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  err = i2s_channel_enable(this->rx_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_enable (RX) failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  if (this->pa_pin_ != GPIO_NUM_NC) {
    gpio_set_direction(this->pa_pin_, GPIO_MODE_OUTPUT);
    gpio_set_level(this->pa_pin_, 1);
    ESP_LOGD(TAG, "PA enabled on GPIO%d", (int) this->pa_pin_);
  }

  ESP_LOGI(TAG, "Duplex I2S ready: %u Hz, TX=std (GPIO%d), RX=TDM 4-slot (GPIO%d), MCLK=GPIO%d",
           this->sample_rate_, (int) this->dout_pin_, (int) this->din_pin_,
           (int) this->mclk_pin_);
}

// ES7210 SDP interface 2 (0x12), bit 1 = TDM enable. ESPHome's es7210 driver
// always writes 0x00 here in setup() ("TDM is unsupported in ESPHome"), so the
// mic task re-enables it once the component tree is up.
bool WaveshareDuplexAudio::enable_es7210_tdm() {
  uint8_t reg01 = 0, reg11 = 0, reg12 = 0;
  if (!this->read_byte(0x01, &reg01) || !this->read_byte(0x11, &reg11) ||
      !this->read_byte(0x12, &reg12)) {
    ESP_LOGE(TAG, "ES7210: cannot read regs 0x01/0x11/0x12");
    return false;
  }

  // reg 0x01 = ADC clock-off. ESPHome's driver only clears 0x0b (ADC1/2), so
  // ADC3/ADC4 stay gated; esp_codec_dev clears 0x15 when MIC3/MIC4 are used.
  // Our AEC reference lives on MIC3, so this is what makes it audible at all.
  // reg 0x12 bit 1 = TDM 4-slot (all four ADCs on SDOUT1).
  const uint8_t want01 = reg01 & (uint8_t) ~0x15;
  const uint8_t want12 = reg12 | 0x02;

  if (reg01 == want01 && reg12 == want12) {
    return true;
  }
  if (!this->write_byte(0x01, want01) || !this->write_byte(0x12, want12)) {
    ESP_LOGE(TAG, "ES7210: cannot write regs 0x01/0x12");
    return false;
  }
  ESP_LOGI(TAG, "ES7210 for AEC: 0x01 0x%02X->0x%02X (ADC clocks on, was gating MIC3/4), "
                "0x12 0x%02X->0x%02X (TDM 4-slot), 0x11=0x%02X — slot order MIC1, MIC3, MIC2, MIC4",
           reg01, want01, reg12, want12, reg11);
  return true;
}

void WaveshareDuplexAudio::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Waveshare Duplex Audio:\n"
                "  Sample rate: %" PRIu32 " Hz\n"
                "  MCLK: GPIO%d\n"
                "  BCLK: GPIO%d\n"
                "  LRCK: GPIO%d\n"
                "  DOUT (speaker): GPIO%d\n"
                "  DIN  (mic):     GPIO%d",
                this->sample_rate_,
                (int) this->mclk_pin_,
                (int) this->bclk_pin_,
                (int) this->lrck_pin_,
                (int) this->dout_pin_,
                (int) this->din_pin_);
  if (this->pa_pin_ != GPIO_NUM_NC) {
    ESP_LOGCONFIG(TAG, "  PA: GPIO%d", (int) this->pa_pin_);
  }
  if (this->is_failed()) {
    ESP_LOGCONFIG(TAG, "  FAILED to initialize!");
  }
}

}  // namespace waveshare_duplex_audio
}  // namespace esphome

#endif  // USE_ESP32
