#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/i2c/i2c.h"

#include <driver/i2s_std.h>
#include <driver/i2s_tdm.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>

#include <atomic>
#include <string>

namespace esphome {
namespace waveshare_duplex_audio {

/**
 * WaveshareDuplexAudio — parent component.
 *
 * Creates a single I2S_NUM_0 duplex channel at setup():
 *   TX → standard I2S (ES8311 speaker, ESP32 master)
 *   RX → TDM 4-slot (ES7210 mic + hardware echo reference)
 *
 * RX slot order is dictated by the ES7210 in TDM mode:
 *
 *   slot 0 = MIC1  board microphone            → mic
 *   slot 1 = MIC3  hardware AEC reference      → ref
 *   slot 2 = MIC2  second board microphone     → mic 2
 *   slot 3 = MIC4  unused / not routed
 *
 * MIC3 is fed from the speaker amplifier output through the on-board
 * "AEC" resistor divider, so the echo reference is analogue, full-band and
 * inherently delay-aligned with what the speaker actually plays. This is the
 * same reference the vendor firmware (xiaozhi) uses, and it removes the need
 * to guess a delay/gain for a software copy of the playback stream.
 *
 * Both channels are enabled immediately so the microphone clock is
 * always present from boot, fixing wake-word detection.
 *
 * The software reference ring (push_aec_ref/pop_ref_samples) is kept as a
 * fallback that can be selected at runtime; it is no longer the default.
 */
class WaveshareDuplexAudio : public Component, public i2c::I2CDevice {
 public:
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void setup() override;
  void dump_config() override;

  void set_mclk_pin(int pin) { this->mclk_pin_ = (gpio_num_t) pin; }
  void set_bclk_pin(int pin) { this->bclk_pin_ = (gpio_num_t) pin; }
  void set_lrck_pin(int pin) { this->lrck_pin_ = (gpio_num_t) pin; }
  void set_dout_pin(int pin) { this->dout_pin_ = (gpio_num_t) pin; }
  void set_din_pin(int pin)  { this->din_pin_  = (gpio_num_t) pin; }
  void set_pa_pin(int pin)   { this->pa_pin_   = (gpio_num_t) pin; }
  void set_sample_rate(uint32_t sr) { this->sample_rate_ = sr; }

  i2s_chan_handle_t get_tx_handle() const { return this->tx_handle_; }
  i2s_chan_handle_t get_rx_handle() const { return this->rx_handle_; }
  uint32_t          get_sample_rate() const { return this->sample_rate_; }

  // The voice stack (ESP-SR AFE, micro_wake_word, voice_assistant, the STT
  // stream) is fixed at 16 kHz. When the I2S bus runs faster for playback
  // quality (48 kHz), the microphone decimates by this integer ratio and the
  // software AEC reference is decimated to match, so everything downstream of
  // the microphone still sees 16 kHz. Ratio is 1 on a 16 kHz bus.
  static constexpr uint32_t VOICE_SAMPLE_RATE = 16000;
  uint32_t get_decimation_ratio() const {
    return (this->sample_rate_ >= VOICE_SAMPLE_RATE &&
            this->sample_rate_ % VOICE_SAMPLE_RATE == 0)
               ? this->sample_rate_ / VOICE_SAMPLE_RATE
               : 1;
  }

  // Number of channels in one RX frame (TDM 4-slot).
  static constexpr int RX_CHANNELS = 4;
  // Number of slots in one TX frame. The speaker runs on the same 4-slot TDM
  // frame as the mic: slot 0 = left, slot 2 = right, slots 1/3 silent.
  static constexpr int TX_CHANNELS = 4;
  // RX slot indices — ES7210 TDM order is MIC1, MIC3, MIC2, MIC4.
  static constexpr int RX_SLOT_MIC1 = 0;  // board microphone
  static constexpr int RX_SLOT_REF = 1;   // MIC3 — hardware echo reference
  static constexpr int RX_SLOT_MIC2 = 2;  // second board microphone

  // Put the ES7210 into TDM 4-slot mode (SDP interface reg 0x12 bit 1) AND
  // un-gate the ADC3/ADC4 clocks (reg 0x01). ESPHome's es7210 driver only ever
  // clears 0x0b in reg 0x01, which leaves ADC3/4 clocked off, so the hardware
  // AEC reference (MIC3, TDM slot 1) reads as pure digital silence and the AFE
  // has nothing to cancel with. esp_codec_dev clears 0x15 for MIC3/MIC4.
  // Must run after the es7210 component's setup(), so it is called repeatedly
  // (mic task start and every AFE (re)creation) and is idempotent.
  bool enable_es7210_tdm();

  // AEC pre-buffer ring API (software reference — fallback only).
  //
  // The hardware reference (RX slot 1, ES7210 MIC3) is the default; this ring
  // is only consumed when the mic component's reference source is "software".
  // Pre-fill advance: init_aec_ref() writes AEC_ADVANCE zeros ahead of the read
  // pointer so the software reference leads the acoustic echo by a fixed amount.
  void     init_aec_ref();
  void     push_aec_ref(const int16_t *stereo, size_t frames);
  void     pop_ref_samples(int16_t *out, size_t n, uint32_t offset = 0);
  uint32_t get_aec_ref_fill() const {
    return aec_ref_wr_.load(std::memory_order_acquire) -
           aec_ref_rd_.load(std::memory_order_relaxed);
  }

  // AEC constants — public for mic task delay calibration
  static constexpr uint32_t AEC_REF_SIZE  = 4096;   // must be power of 2
  static constexpr uint32_t AEC_ADVANCE   = 1024;   // pre-buffer lead in samples
  
 protected:
  gpio_num_t mclk_pin_{I2S_GPIO_UNUSED};
  gpio_num_t bclk_pin_{I2S_GPIO_UNUSED};
  gpio_num_t lrck_pin_{I2S_GPIO_UNUSED};
  gpio_num_t dout_pin_{I2S_GPIO_UNUSED};
  gpio_num_t din_pin_{I2S_GPIO_UNUSED};
  gpio_num_t pa_pin_{GPIO_NUM_NC};

  uint32_t sample_rate_{16000};

  i2s_chan_handle_t tx_handle_{nullptr};
  i2s_chan_handle_t rx_handle_{nullptr};

  // AEC reference ring — lock-free SPSC, power-of-2 size.
  // 4096 samples = 256 ms at 16 kHz.
  // Write pointer leads read pointer by AEC_ADVANCE samples at TTS onset so
  // mic always pops a reference that precedes the acoustic echo by the right
  // amount (optimal NLMS tap ≈ D_dma + D_acoustic − AEC_ADVANCE ≈ 418).
  int16_t               aec_ref_ring_[AEC_REF_SIZE]{};
  std::atomic<uint32_t> aec_ref_wr_{0};
  std::atomic<uint32_t> aec_ref_rd_{0};
};

}  // namespace waveshare_duplex_audio
}  // namespace esphome

#endif  // USE_ESP32
