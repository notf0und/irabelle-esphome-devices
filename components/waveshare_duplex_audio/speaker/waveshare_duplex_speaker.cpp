#include "waveshare_duplex_speaker.h"

#ifdef USE_ESP32

#include <driver/i2s_std.h>
#include <esp_timer.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/audio/audio.h"

namespace esphome {
namespace waveshare_duplex_audio {

static const char *const SPK_TAG = "waveshare_duplex.spk";

// DMA buffer write timeout
static const uint32_t DMA_WRITE_TIMEOUT_MS = 20;

// Ring-buffer read timeout: how long the task waits for audio data before stopping
static const uint32_t RING_READ_TIMEOUT_MS = 20;

// FreeRTOS task parameters
static const size_t    SPK_TASK_STACK    = 4096;
static const UBaseType_t SPK_TASK_PRIO  = 19;

enum SpkEventBits : uint32_t {
  SPK_CMD_START            = (1 << 0),
  SPK_CMD_STOP             = (1 << 1),
  SPK_CMD_STOP_GRACEFULLY  = (1 << 2),

  SPK_TASK_STARTING = (1 << 10),
  SPK_TASK_RUNNING  = (1 << 11),
  SPK_TASK_STOPPING = (1 << 12),
  SPK_TASK_STOPPED  = (1 << 13),

  SPK_ALL_BITS = 0x00FFFFFF,
};

// ─────────────────────────────────────────────────────────────────────────────
// Component lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void WaveshareDuplexSpeaker::setup() {
  this->event_group_ = xEventGroupCreate();
  if (!this->event_group_) {
    ESP_LOGE(SPK_TAG, "Failed to create event group");
    this->mark_failed();
    return;
  }
  // Reflect the configured sample rate into the stream info
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 2, this->parent_->get_sample_rate());

  // Allocate the output ring buffer NOW, at boot, while PSRAM is still one big
  // unfragmented block. It used to be allocated lazily from play() - i.e. at the
  // moment the first station was started, after the media pipeline had already
  // taken its own buffers. On this board the heap was tight enough then that the
  // 192 kB request failed; play() returned 0, the mixer task spun on a sink that
  // accepted nothing and the task watchdog rebooted the device. Doing it here
  // makes the buffer's existence independent of streaming-time fragmentation.
  this->ensure_ring_buffer_();
}

bool WaveshareDuplexSpeaker::ensure_ring_buffer_() {
  if (this->audio_ring_buffer_)
    return true;

  const size_t ring_bytes = this->audio_stream_info_.ms_to_bytes(this->buffer_duration_ms_);
  this->audio_ring_buffer_ = ring_buffer::RingBuffer::create(ring_bytes);
  if (!this->audio_ring_buffer_) {
    ESP_LOGE(SPK_TAG, "Failed to allocate ring buffer (%zu bytes)", ring_bytes);
    return false;
  }
  ESP_LOGI(SPK_TAG, "Ring buffer allocated: %zu bytes", ring_bytes);
  return true;
}

void WaveshareDuplexSpeaker::dump_config() {
  ESP_LOGCONFIG(SPK_TAG,
                "Waveshare Duplex Speaker:\n"
                "  Sample rate: %" PRIu32 " Hz\n"
                "  Buffer duration: %" PRIu32 " ms",
                this->parent_->get_sample_rate(), this->buffer_duration_ms_);
}

// ─────────────────────────────────────────────────────────────────────────────
// speaker::Speaker interface
// ─────────────────────────────────────────────────────────────────────────────

void WaveshareDuplexSpeaker::start() {
  if (!this->is_ready() || this->is_failed()) return;
  if (this->state_ == speaker::STATE_STARTING || this->state_ == speaker::STATE_RUNNING) return;
  xEventGroupSetBits(this->event_group_, SpkEventBits::SPK_CMD_START);
}

void WaveshareDuplexSpeaker::stop()   { this->stop_(false); }
void WaveshareDuplexSpeaker::finish() { this->stop_(true); }

void WaveshareDuplexSpeaker::stop_(bool graceful) {
  if (this->is_failed()) return;
  if (this->state_ == speaker::STATE_STOPPED) return;
  xEventGroupSetBits(this->event_group_,
                     graceful ? SpkEventBits::SPK_CMD_STOP_GRACEFULLY
                              : SpkEventBits::SPK_CMD_STOP);
}

size_t WaveshareDuplexSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_failed()) return 0;

  // Normally already allocated in setup(); this is a safety net.
  if (!this->ensure_ring_buffer_()) {
    // Returning 0 immediately makes the mixer task loop without ever blocking
    // (the mixer only delays when a source has no data), which starves the idle
    // task and resets the chip through the task watchdog. Back off instead so a
    // memory problem degrades to silence rather than a reboot.
    if (!xPortInIsrContext())
      vTaskDelay(pdMS_TO_TICKS(10));
    return 0;
  }

  this->start();
  // Use write_without_replacement so the mixer is blocked (back-pressure) when the ring buffer is full,
  // rather than silently overwriting audio data.
  return this->audio_ring_buffer_->write_without_replacement(data, length, ticks_to_wait);
}

bool WaveshareDuplexSpeaker::has_buffered_data() const {
  return this->audio_ring_buffer_ && this->audio_ring_buffer_->available() > 0;
}

uint32_t WaveshareDuplexSpeaker::get_buffered_ms() const {
  if (this->audio_ring_buffer_ == nullptr)
    return 0;
  const uint32_t sample_rate = this->get_output_sample_rate();
  // The ring holds stereo 16-bit frames at the parent's sample rate.
  const size_t bytes_per_ms = (size_t) sample_rate * 2 * sizeof(int16_t) / 1000;
  if (bytes_per_ms == 0)
    return 0;
  return (uint32_t) (this->audio_ring_buffer_->available() / bytes_per_ms);
}

void WaveshareDuplexSpeaker::set_volume(float volume) {
  this->volume_ = volume;
#ifdef USE_AUDIO_DAC
  if (this->audio_dac_) {
    this->audio_dac_->set_volume(volume);
    return;
  }
#endif
  // Software Q15 volume when no DAC component is configured
  const float clamped = std::max(0.0f, std::min(1.0f, volume));
  this->q15_volume_   = (int16_t)(clamped * INT16_MAX);
}

void WaveshareDuplexSpeaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
#ifdef USE_AUDIO_DAC
  if (this->audio_dac_) {
    if (mute_state) this->audio_dac_->set_mute_on();
    else            this->audio_dac_->set_mute_off();
    return;
  }
#endif
  this->q15_volume_ = mute_state ? 0 : (int16_t)(this->volume_ * INT16_MAX);
}

// ─────────────────────────────────────────────────────────────────────────────
// loop() — state machine driven by event group bits
// ─────────────────────────────────────────────────────────────────────────────

void WaveshareDuplexSpeaker::loop() {
  if (this->is_failed()) return;

  uint32_t bits = xEventGroupGetBits(this->event_group_);

  if (bits & SpkEventBits::SPK_CMD_START && this->state_ == speaker::STATE_STOPPED) {
    this->state_ = speaker::STATE_STARTING;
    xEventGroupClearBits(this->event_group_, SpkEventBits::SPK_CMD_START);
  }
  if (bits & SpkEventBits::SPK_TASK_RUNNING) {
    xEventGroupClearBits(this->event_group_, SpkEventBits::SPK_TASK_RUNNING);
    this->state_ = speaker::STATE_RUNNING;
  }
  if (bits & SpkEventBits::SPK_TASK_STOPPING) {
    xEventGroupClearBits(this->event_group_, SpkEventBits::SPK_TASK_STOPPING);
    this->state_ = speaker::STATE_STOPPING;
  }
  if (bits & SpkEventBits::SPK_TASK_STOPPED) {
    vTaskDelete(this->speaker_task_handle_);
    this->speaker_task_handle_ = nullptr;
    xEventGroupClearBits(this->event_group_, SpkEventBits::SPK_ALL_BITS);
    this->state_ = speaker::STATE_STOPPED;
  }

  switch (this->state_) {
    case speaker::STATE_STARTING:
      if (!this->speaker_task_handle_) {
        xTaskCreate(WaveshareDuplexSpeaker::speaker_task, "wsdpx_spk",
                    SPK_TASK_STACK, this, SPK_TASK_PRIO, &this->speaker_task_handle_);
        if (!this->speaker_task_handle_) {
          ESP_LOGE(SPK_TAG, "Failed to create speaker task");
          this->state_ = speaker::STATE_STOPPED;
        }
      }
      break;
    default:
      break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// FreeRTOS speaker task
// ─────────────────────────────────────────────────────────────────────────────

void WaveshareDuplexSpeaker::speaker_task(void *params) {
  auto *self = static_cast<WaveshareDuplexSpeaker *>(params);
  xEventGroupSetBits(self->event_group_, SpkEventBits::SPK_TASK_STARTING);

  i2s_chan_handle_t tx = self->parent_->get_tx_handle();
  ESP_LOGI(SPK_TAG, "Speaker task started, tx_handle=%p", tx);

  if (!tx) {
    ESP_LOGE(SPK_TAG, "tx_handle is null — aborting speaker task");
    xEventGroupSetBits(self->event_group_, SpkEventBits::SPK_TASK_STOPPING);
    xEventGroupSetBits(self->event_group_, SpkEventBits::SPK_TASK_STOPPED);
    while (true) vTaskDelay(pdMS_TO_TICKS(10));
  }

  // Ring buffer data is stereo 16-bit; the TX runs 4-slot TDM, so each frame
  // is expanded to [L, 0, R, 0] on write (see the parent component). The
  // ES8311 latches slots 0 and 2 and ignores the silent slots.
  const uint32_t sample_rate = self->parent_->get_sample_rate();
  const size_t   buf_frames  = (sample_rate * DMA_WRITE_TIMEOUT_MS) / 1000;
  const size_t   buf_bytes   = buf_frames * 2 * sizeof(int16_t);  // stereo input
  std::vector<uint8_t> write_buf(buf_bytes);
  std::vector<int16_t> tdm_buf(buf_frames * WaveshareDuplexAudio::TX_CHANNELS);

  xEventGroupSetBits(self->event_group_, SpkEventBits::SPK_TASK_RUNNING);

  uint32_t last_data_ms  = millis();
  bool     timed_out     = false;

  while (true) {
    uint32_t bits = xEventGroupGetBits(self->event_group_);

    // Hard stop
    if (bits & SpkEventBits::SPK_CMD_STOP) break;

    // Graceful stop: drain ring buffer then exit
    if (bits & SpkEventBits::SPK_CMD_STOP_GRACEFULLY) {
      if (!self->audio_ring_buffer_ || self->audio_ring_buffer_->available() == 0) break;
    }

    // Timeout (no data for too long)
    if (self->timeout_ms_ > 0 && (millis() - last_data_ms) >= self->timeout_ms_) {
      timed_out = true;
      break;
    }

    if (!self->audio_ring_buffer_ || self->audio_ring_buffer_->available() == 0) {
      vTaskDelay(pdMS_TO_TICKS(RING_READ_TIMEOUT_MS));
      continue;
    }

    // Read from ring buffer
    size_t to_read    = std::min(self->audio_ring_buffer_->available(), buf_bytes);
    size_t bytes_read = self->audio_ring_buffer_->read(write_buf.data(), to_read, 0);

    if (bytes_read == 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    last_data_ms = millis();

    // Apply Q15 software volume if < full volume and no hardware DAC
    const int16_t q15 = self->q15_volume_;
    if (q15 < INT16_MAX) {
      int16_t *samples = reinterpret_cast<int16_t *>(write_buf.data());
      const size_t n   = bytes_read / sizeof(int16_t);
      for (size_t i = 0; i < n; ++i) {
        samples[i] = (int16_t)(((int32_t) samples[i] * q15) >> 15);
      }
    }

    // Push AEC reference AFTER volume scaling, BEFORE DMA write.
    // At TTS onset init_aec_ref() pre-fills AEC_ADVANCE zeros so the mic task
    // always reads a reference that leads the acoustic echo by the right amount,
    // giving NLMS a causal (non-negative) tap to converge on (~418 samples).
    {
      const int16_t *stereo = reinterpret_cast<const int16_t *>(write_buf.data());
      const size_t frames   = bytes_read / (2 * sizeof(int16_t));
      float rms_ref = 0.0f;
      const size_t n = (frames <= 320) ? frames : 320;
      for (size_t i = 0; i < n; ++i) {
        float s = (float) stereo[i * 2];
        rms_ref += s * s;
      }
      rms_ref = sqrtf(rms_ref / n);

      static bool spk_was_silent = true;
      static int  silence_chunks = 0;
      if (spk_was_silent && rms_ref > 200.0f) {
        // Pre-fill AEC_ADVANCE zeros, then arm the write pointer, so the
        // software reference leads the echo (only used when AEC Ref Source is
        // "software"; the default hardware reference needs none of this).
        self->parent_->init_aec_ref();
        ESP_LOGD(SPK_TAG, "TTS onset: RMS=%.0f (software AEC ref armed)", rms_ref);
        spk_was_silent  = false;
        silence_chunks  = 0;
      } else if (!spk_was_silent) {
        if (rms_ref < 50.0f) {
          // Require ~450 ms of continuous silence before declaring TTS over.
          // This prevents inter-word pauses from triggering a spurious reset.
          if (++silence_chunks >= 30) {
            spk_was_silent = true;
            silence_chunks = 0;
          }
        } else {
          silence_chunks = 0;  // non-silent chunk resets the hold-off
        }
      }

      // Always push current chunk (zeros during silence are correct — mic
      // should get silence reference when no audio is playing).
      self->parent_->push_aec_ref(stereo, frames);
    }

    // Write to I2S — the channel is always enabled by the parent.
    // Re-pack stereo → 4-slot TDM: [L, 0, R, 0] per frame.
    const int16_t *in = reinterpret_cast<const int16_t *>(write_buf.data());
    const size_t frames_in = bytes_read / (2 * sizeof(int16_t));
    for (size_t i = 0; i < frames_in; ++i) {
      int16_t *dst = tdm_buf.data() + i * WaveshareDuplexAudio::TX_CHANNELS;
      dst[0] = in[i * 2];
      dst[1] = 0;
      dst[2] = in[i * 2 + 1];
      dst[3] = 0;
    }
    size_t bytes_written = 0;
    const size_t tdm_bytes = frames_in * WaveshareDuplexAudio::TX_CHANNELS * sizeof(int16_t);
    esp_err_t err = i2s_channel_write(tx, tdm_buf.data(), tdm_bytes,
                                      &bytes_written, pdMS_TO_TICKS(DMA_WRITE_TIMEOUT_MS));
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
      ESP_LOGW(SPK_TAG, "i2s_channel_write error: %s", esp_err_to_name(err));
    }

    // Notify the mixer how many frames were played so it can track pipeline depth.
    if (bytes_written > 0) {
      const size_t frame_size = WaveshareDuplexAudio::TX_CHANNELS * sizeof(int16_t);
      const uint32_t frames = bytes_written / frame_size;
      // Advance the playback clock before notifying, so a callback that reads it
      // (and the caption sync in the main loop) always sees the frames it was
      // just told about.
      self->played_frames_.fetch_add(frames, std::memory_order_relaxed);
      self->audio_output_callback_.call(frames, esp_timer_get_time());
    }
  }

  if (timed_out) {
    ESP_LOGD(SPK_TAG, "Speaker timed out (no data for %" PRIu32 " ms)", self->timeout_ms_);
  }

  xEventGroupSetBits(self->event_group_, SpkEventBits::SPK_TASK_STOPPING);
  xEventGroupSetBits(self->event_group_, SpkEventBits::SPK_TASK_STOPPED);

  while (true) vTaskDelay(pdMS_TO_TICKS(10));
}

}  // namespace waveshare_duplex_audio
}  // namespace esphome

#endif  // USE_ESP32
