#pragma once

#ifdef USE_ESP32

#include "../waveshare_duplex_audio.h"

#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"

#ifdef USE_AUDIO_DAC
#include "esphome/components/audio_dac/audio_dac.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <driver/i2s_std.h>

namespace esphome {
namespace waveshare_duplex_audio {

/**
 * WaveshareDuplexSpeaker — speaker platform.
 *
 * Writes audio data to the parent's TX handle (standard I2S, always on).
 * No i2s_new_channel / i2s_del_channel calls — the I2S channel is owned by
 * the parent and is never torn down.
 *
 * Audio pipeline:
 *   play() → ring buffer → speaker_task → i2s_channel_write(tx_handle)
 */
class WaveshareDuplexSpeaker : public speaker::Speaker, public Component {
 public:
  float get_setup_priority() const override { return setup_priority::LATE; }

  void setup() override;
  void dump_config() override;
  void loop() override;

  void set_parent(WaveshareDuplexAudio *parent) { this->parent_ = parent; }
  void set_buffer_duration(uint32_t ms) { this->buffer_duration_ms_ = ms; }
  void set_timeout(uint32_t ms)        { this->timeout_ms_ = ms; }
#ifdef USE_AUDIO_DAC
  void set_audio_dac(audio_dac::AudioDac *dac) { this->audio_dac_ = dac; }
#endif

  // speaker::Speaker interface
  void   start() override;
  void   stop() override;
  void   finish() override;
  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return play(data, length, 0); }
  bool   has_buffered_data() const override;

  void  set_volume(float volume) override;
  void  set_mute_state(bool mute_state) override;

  // ── Playback clock ───────────────────────────────────────────────────────
  // Cumulative stereo frames actually handed to the I2S DAC since boot. Frames
  // only advance while the DAC is being fed, so this is a real playback clock
  // (what is being *heard*), not a queue counter. The voice caption logic uses
  // it to keep on-screen text in step with the spoken audio — see the
  // "Voice caption" interval in waveshare-amoled-216.yaml.
  uint64_t get_played_frames() const {
    return this->played_frames_.load(std::memory_order_relaxed);
  }
  uint32_t get_output_sample_rate() const {
    return this->parent_ != nullptr ? this->parent_->get_sample_rate() : 16000;
  }
  // Milliseconds of audio already queued in the output ring, i.e. how much is
  // still to be played. The media player reports an announcement as "finished"
  // as soon as the announcement pipeline has drained, which is up to a full
  // ring (buffer_duration, 1000 ms) before the DAC runs out — so the caption
  // uses this to hold the text until the queued tail has actually been played.
  uint32_t get_buffered_ms() const;

 protected:
  static void speaker_task(void *params);
  void stop_(bool graceful);
  /// Allocates the output ring buffer if it does not exist yet.
  ///
  /// Called eagerly from setup() (PSRAM is one big, unfragmented block at boot)
  /// and again from play(); if it ever fails, play() backs off instead of
  /// letting the mixer hot-loop into a task-watchdog reset.
  bool ensure_ring_buffer_();

  WaveshareDuplexAudio *parent_{nullptr};

  EventGroupHandle_t event_group_{nullptr};
  TaskHandle_t speaker_task_handle_{nullptr};

  std::shared_ptr<ring_buffer::RingBuffer> audio_ring_buffer_;

  uint32_t buffer_duration_ms_{500};
  uint32_t timeout_ms_{0};  // 0 = no timeout
  float    volume_{1.0f};

  // Q15 fixed-point volume factor (INT16_MAX = full volume)
  int16_t q15_volume_{INT16_MAX};

  // Monotonic DAC frame counter — see get_played_frames(). Written by the
  // speaker task, read from the main loop (caption sync), hence the atomic.
  std::atomic<uint64_t> played_frames_{0};

#ifdef USE_AUDIO_DAC
  audio_dac::AudioDac *audio_dac_{nullptr};
#endif
};

}  // namespace waveshare_duplex_audio
}  // namespace esphome

#endif  // USE_ESP32
