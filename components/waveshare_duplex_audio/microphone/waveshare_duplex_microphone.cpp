#include "waveshare_duplex_microphone.h"

#ifdef USE_ESP32

#include <driver/i2s_std.h>
#include <esp_aec.h>
#include <esp_aec_nlp.h>
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "model_path.h"
#include <esp_heap_caps.h>
#include <esp_partition.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/audio/audio.h"

#include <cstring>
#include <vector>
#include <cmath>


namespace esphome {
namespace waveshare_duplex_audio {

static const char *const MIC_TAG = "waveshare_duplex.mic";

// Optional embedded ESP-SR model blob: scripts/pack-models.sh --emit-blob
// writes srmodels_blob.cpp next to the component, and the Python side defines
// USE_SRMODELS_BLOB when that file exists (the reference must be strong, as a
// weak one would not pull the archive member out of libsrc.a).
#ifdef USE_SRMODELS_BLOB
extern "C" {
extern const uint8_t srmodels_blob[];
extern const uint8_t srmodels_blob_end[];
}
#endif

static const size_t       MIC_TASK_STACK    = 16384;
static const UBaseType_t  MIC_TASK_PRIORITY = 5;
// One RX frame carries the full ES7210 TDM group: MIC1, MIC3(ref), MIC2, MIC4.
static const uint32_t     RX_CHANNELS       = WaveshareDuplexAudio::RX_CHANNELS;
static const uint32_t     CH_MIC1           = WaveshareDuplexAudio::RX_SLOT_MIC1;
static const uint32_t     CH_REF            = WaveshareDuplexAudio::RX_SLOT_REF;
static const uint32_t     CH_MIC2           = WaveshareDuplexAudio::RX_SLOT_MIC2;

// Decimate one raw TDM frame from the I2S bus rate down to the voice rate, in
// place. `buf` holds in_frames * RX_CHANNELS interleaved int16 samples and the
// result is the first out_frames * RX_CHANNELS entries. Each output sample is
// the mean of `ratio` consecutive input samples: a box low-pass whose DC gain is
// 1. It is safe in place because output j is written at index j*RX_CHANNELS+c
// while it reads from (j*ratio)*RX_CHANNELS+c onward, i.e. never behind the
// write cursor. ratio == 1 is a no-op (16 kHz bus keeps the old code path).
static void decimate_tdm_in_place(int16_t *buf, size_t in_frames, size_t out_frames, uint32_t ratio) {
  if (ratio <= 1) return;
  (void) in_frames;
  for (size_t j = 0; j < out_frames; ++j) {
    const size_t base = j * ratio;
    for (uint32_t c = 0; c < RX_CHANNELS; ++c) {
      int32_t acc = 0;
      for (uint32_t k = 0; k < ratio; ++k)
        acc += buf[(base + k) * RX_CHANNELS + c];
      buf[j * RX_CHANNELS + c] = (int16_t) (acc / (int32_t) ratio);
    }
  }
}

void WaveshareDuplexMicrophone::setup() {
  // Published to the voice stack at the 16 kHz voice rate even when the I2S bus
  // runs faster for playback quality; the mic task decimates to match.
  this->audio_stream_info_ = audio::AudioStreamInfo(
      16, 1, this->output_sample_rate_);

  BaseType_t ret = xTaskCreate(
      WaveshareDuplexMicrophone::mic_task,
      "wsdpx_mic",
      MIC_TASK_STACK,
      this,
      MIC_TASK_PRIORITY,
      &this->task_handle_);

  if (ret != pdPASS || !this->task_handle_) {
    ESP_LOGE(MIC_TAG, "Failed to create mic task (ret=%d)", (int) ret);
    this->mark_failed();
  } else {
    ESP_LOGI(MIC_TAG,
             "Mic task created (aec_gain=%.1f, bus=%" PRIu32 " Hz, voice=%" PRIu32 " Hz, decimate %" PRIu32 ":1)",
             this->aec_gain_.load(), this->parent_->get_sample_rate(), this->output_sample_rate_,
             this->parent_->get_decimation_ratio());
  }
}

void WaveshareDuplexMicrophone::dump_config() {
  const char *nlp_names[] = {"normal", "aggressive", "very_aggressive"};
  int nlp = this->aec_nlp_target_.load();
  
  ESP_LOGCONFIG(MIC_TAG,
                "Waveshare Duplex Microphone:\n"
                "  Sample rate: %" PRIu32 " Hz\n"
                "  AEC gain: %.1f\n"
                "  AEC NLP: %s\n"
                "  AEC mode: %d\n"
                "  AEC filter length: %d\n"
                "  Mic mode: %d (%s)\n"
                "  AEC ref source: %s\n"
                "  Audio frontend: %s (%s)",
                this->parent_->get_sample_rate(), this->aec_gain_.load(),
                (nlp >= 0 && nlp <= 2) ? nlp_names[nlp] : "unknown", this->aec_mode_.load(),
                this->aec_filter_length_.load(), this->get_mic_mode(),
                mic_mode_is_dual(this->get_mic_mode()) ? "dual" : "single",
                ref_source_name(this->get_ref_source()),
                frontend_name(this->get_audio_frontend()),
                this->get_audio_frontend() == FRONTEND_AFE ? afe_kind_name(this->get_afe_kind()) : "n/a");
}

// Periodic status readback at DEBUG (the yaml's global level is INFO, so this
// only appears with the "Log Level" select raised). Boot-time logs happen before
// WiFi/API is up, which is why the state is re-emitted for a while at all.
void WaveshareDuplexMicrophone::loop() {
  static uint32_t last_ms = 0;
  uint32_t now = millis();
  // 5 s cadence for the first 20 s, then every 60 s.
  uint32_t period = (this->status_logs_ < 4) ? 5000 : 60000;
  if (last_ms != 0 && (now - last_ms) < period)
    return;
  last_ms = now;
  this->status_logs_++;

  ESP_LOGD(MIC_TAG,
           "Audio path: frontend=%s/%s ref=%s tdm=%s rx_frames=%" PRIu32 " rx_empty=%" PRIu32
           " rms=[%.0f %.0f %.0f %.0f] in=%.0f out=%.0f afe_feed=%d ch=%d produced=%" PRIu32
           " retries=%d models=%d listeners=%d vad=%s barge=%" PRIu32, 
           frontend_name(this->get_audio_frontend()),
           this->get_audio_frontend() == FRONTEND_AFE ? afe_kind_name(this->get_afe_kind()) : "n/a",
           ref_source_name(this->get_ref_source()),
           this->tdm_state_.load() == 1 ? "on" : (this->tdm_state_.load() < 0 ? "FAILED" : "pending"),
           this->rx_frames_.load(), this->rx_empty_.load(),
           this->ch_rms_[0].load(), this->ch_rms_[1].load(),
           this->ch_rms_[2].load(), this->ch_rms_[3].load(),
           this->aec_in_rms_.load(), this->aec_out_rms_.load(),
           this->afe_feed_n_.load(), this->afe_ch_num_.load(), this->afe_produced_.load(),
           this->afe_retries_.load(), this->afe_have_models_.load(),
           this->active_listeners_.load(),
           this->speech_events_.load());
}

void WaveshareDuplexMicrophone::start() {
  if (this->is_failed()) return;
  int prev = this->active_listeners_.fetch_add(1);
  if (prev == 0) {
    this->state_ = microphone::STATE_RUNNING;
    ESP_LOGD(MIC_TAG, "Microphone started (first listener)");
  }
}

void WaveshareDuplexMicrophone::stop() {
  if (this->is_failed()) return;
  int prev = this->active_listeners_.fetch_sub(1);
  if (prev <= 1) {
    if (this->active_listeners_.load() < 0)
      this->active_listeners_.store(0);
    this->state_ = microphone::STATE_STOPPED;
    ESP_LOGD(MIC_TAG, "Microphone stopped (no listeners)");
  }
}

void WaveshareDuplexMicrophone::set_aec_mode(int mode) {
  switch (mode) {
    case AEC_MODE_SR_LOW_COST:
    case AEC_MODE_SR_HIGH_PERF:
    case AEC_MODE_VOIP_LOW_COST:
    case AEC_MODE_VOIP_HIGH_PERF:
    case AEC_MODE_FD_LOW_COST:
    case AEC_MODE_FD_HIGH_PERF:
      break;
    default:
      ESP_LOGW(MIC_TAG, "Ignoring invalid AEC mode: %d", mode);
      return;
  }
  int prev = this->aec_mode_.exchange(mode);
  if (prev != mode) this->aec_config_gen_.fetch_add(1);
}

void WaveshareDuplexMicrophone::set_aec_filter_length(int length) {
  if (length < 1 || length > 32) {
    ESP_LOGW(MIC_TAG, "Ignoring invalid AEC filter length: %d", length);
    return;
  }
  int prev = this->aec_filter_length_.exchange(length);
  if (prev != length) this->aec_config_gen_.fetch_add(1);
}

void WaveshareDuplexMicrophone::set_aec_ref_offset(int offset) {
  if (offset < 0) return;
  this->aec_ref_offset_.store(offset);
}

void WaveshareDuplexMicrophone::set_aec_ref_gain(float gain) {
  if (gain <= 0.0f) return;
  this->aec_ref_gain_.store(gain);
}

void WaveshareDuplexMicrophone::set_mic_mode(int mode) {
  if (mode < MIC_MODE_MIC1 || mode > MIC_MODE_DUAL_AVG) {
    ESP_LOGW(MIC_TAG, "Ignoring invalid mic mode: %d", mode);
    return;
  }
  int prev = this->mic_mode_.exchange(mode);
  if (prev != mode) {
    // Force the AEC to be recreated with the new channel count.
    this->aec_config_gen_.fetch_add(1);
    ESP_LOGI(MIC_TAG, "Mic mode %d -> %d (AEC will restart)", prev, mode);
  }
}

void WaveshareDuplexMicrophone::set_audio_frontend(int mode) {
  if (mode != FRONTEND_AEC && mode != FRONTEND_AFE) {
    ESP_LOGW(MIC_TAG, "Ignoring invalid audio frontend: %d", mode);
    return;
  }
  int prev = this->audio_frontend_.exchange(mode);
  if (prev != mode) {
    this->aec_config_gen_.fetch_add(1);
    ESP_LOGI(MIC_TAG, "Audio frontend %s -> %s (restarting)", frontend_name(prev), frontend_name(mode));
  }
}

void WaveshareDuplexMicrophone::set_afe_kind(int kind) {
  if (kind < AFE_KIND_SR || kind > AFE_KIND_VC) {
    ESP_LOGW(MIC_TAG, "Ignoring invalid AFE kind: %d", kind);
    return;
  }
  // AFE_TYPE_VC panics inside esp-sr's own VOIP AEC (dios_ssp_aec_firfilter_reset,
  // NULL deref) with "MR" input plus models - reproduced on esp-sr 2.4.3, 2.4.6
  // and 2.5.3. Refuse it rather than taking the device down; FD already includes
  // nonlinear echo suppression. The yaml no longer offers "vc" either.
  if (kind == AFE_KIND_VC) {
    ESP_LOGE(MIC_TAG, "AFE kind 'vc' crashes esp-sr with MR input - keeping %s",
             afe_kind_name(this->afe_kind_.load()));
    return;
  }
  int prev = this->afe_kind_.exchange(kind);
  if (prev != kind) {
    // AFE type is fixed at create time, so force a recreate.
    this->aec_config_gen_.fetch_add(1);
    ESP_LOGI(MIC_TAG, "AFE kind %s -> %s (restarting)", afe_kind_name(prev), afe_kind_name(kind));
  }
}

void WaveshareDuplexMicrophone::set_aec_nlp(int nlp) {
  int prev = this->aec_nlp_target_.exchange(nlp);
  if (prev != nlp && this->get_audio_frontend() == FRONTEND_AFE) {
    // The AFE takes the NLP level at create time.
    this->aec_config_gen_.fetch_add(1);
    ESP_LOGI(MIC_TAG, "AEC NLP %d -> %d (restarting AFE)", prev, nlp);
  }
}

void WaveshareDuplexMicrophone::set_ref_source(int mode) {
  if (mode != REF_SOURCE_HW && mode != REF_SOURCE_SOFTWARE) {
    ESP_LOGW(MIC_TAG, "Ignoring invalid AEC ref source: %d", mode);
    return;
  }
  int prev = this->ref_source_.exchange(mode);
  if (prev != mode) {
    this->aec_config_gen_.fetch_add(1);
    ESP_LOGI(MIC_TAG, "AEC ref source %s -> %s (restarting)", ref_source_name(prev), ref_source_name(mode));
  }
}

void WaveshareDuplexMicrophone::set_afe_ns(bool enabled) {
  bool prev = this->afe_ns_.exchange(enabled);
  if (prev != enabled) {
    this->aec_config_gen_.fetch_add(1);
    ESP_LOGI(MIC_TAG, "AFE NS %s (restarting AFE)", enabled ? "on" : "off");
  }
}

void WaveshareDuplexMicrophone::set_afe_agc(bool enabled) {
  bool prev = this->afe_agc_.exchange(enabled);
  if (prev != enabled) {
    this->aec_config_gen_.fetch_add(1);
    ESP_LOGI(MIC_TAG, "AFE AGC %s (restarting AFE)", enabled ? "on" : "off");
  }
}

void WaveshareDuplexMicrophone::set_barge_in_level(float level) {
  if (level < 20.0f) level = 20.0f;
  if (level > 20000.0f) level = 20000.0f;
  this->barge_in_level_.store(level);
}

void WaveshareDuplexMicrophone::process_with_nlms_(aec_handle_t *aec, int frame_size) {
  const int mic_mode = this->mic_mode_.load();
  const int channels = mic_mode_is_dual(mic_mode) ? 2 : 1;
  const bool hw_ref = this->ref_source_.load() == REF_SOURCE_HW;
  const uint32_t ratio = this->parent_->get_decimation_ratio();
  const size_t read_frames = (size_t) frame_size * (size_t) ratio;
  const size_t frame_bytes = (size_t) frame_size * sizeof(int16_t);
  const size_t ch_bytes = frame_bytes * (size_t) channels;
  const size_t rx_bytes = read_frames * RX_CHANNELS * sizeof(int16_t);

  int16_t *rx_buf = (int16_t *) heap_caps_aligned_alloc(16, rx_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  int16_t *mic_buf = (int16_t *) heap_caps_aligned_alloc(16, ch_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  int16_t *ref_buf = (int16_t *) heap_caps_aligned_alloc(16, frame_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  int16_t *out_buf = (int16_t *) heap_caps_aligned_alloc(16, ch_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  int16_t *mono_buf = (int16_t *) heap_caps_aligned_alloc(16, frame_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (!rx_buf || !mic_buf || !ref_buf || !out_buf || !mono_buf) {
    ESP_LOGE(MIC_TAG, "Failed to allocate aligned AEC buffers (frame=%d, ch=%d)", frame_size, channels);
    if (rx_buf) heap_caps_free(rx_buf);
    if (mic_buf) heap_caps_free(mic_buf);
    if (ref_buf) heap_caps_free(ref_buf);
    if (out_buf) heap_caps_free(out_buf);
    if (mono_buf) heap_caps_free(mono_buf);
    return;
  }
  std::vector<int16_t> aec_out;

  uint32_t read_count = 0;
  int current_nlp = this->aec_nlp_target_.load();
  uint32_t cfg_gen = this->aec_config_gen_.load();

  ESP_LOGI(MIC_TAG, "esp_aec processing (frame=%d, ch=%d, mic_mode=%d, gain=%.1f, nlp=%d, mode=%d, filter=%d, ref_source=%s, offset=%d, ref_gain=%.2f)",
           frame_size, channels, mic_mode, this->aec_gain_.load(), current_nlp, this->aec_mode_.load(),
           this->aec_filter_length_.load(), ref_source_name(hw_ref ? REF_SOURCE_HW : REF_SOURCE_SOFTWARE),
           this->aec_ref_offset_.load(), this->aec_ref_gain_.load());

  while (true) {
    if (this->aec_config_gen_.load() != cfg_gen) {
      ESP_LOGI(MIC_TAG, "esp_aec: config changed, restarting");
      break;
    }

    // Check for runtime NLP changes
    int target_nlp = this->aec_nlp_target_.load();
    if (target_nlp != current_nlp) {
      aec_set_nlp_level(aec, (aec_nlp_level_t)target_nlp);
      current_nlp = target_nlp;
      ESP_LOGI(MIC_TAG, "NLP changed to %d", target_nlp);
    }

    size_t bytes_read = 0;
    i2s_channel_read(this->parent_->get_rx_handle(), rx_buf,
                     rx_bytes, &bytes_read, pdMS_TO_TICKS(200));
    if (bytes_read == 0) {
      this->rx_empty_.fetch_add(1);
      continue;
    }
    this->rx_frames_.fetch_add(1);

    // Bus rate → voice rate (no-op on a 16 kHz bus).
    decimate_tdm_in_place(rx_buf, read_frames, (size_t) frame_size, ratio);

    // Per-slot RMS of the raw TDM frame (diagnostics: slot 1 is the hardware
    // echo reference, so during playback it should track the speaker level).
    for (uint32_t c = 0; c < RX_CHANNELS; ++c) {
      float acc = 0.0f;
      for (size_t i = 0; i < (size_t) frame_size; ++i) {
        float v = (float) rx_buf[i * RX_CHANNELS + c];
        acc += v * v;
      }
      this->ch_rms_[c].store(sqrtf(acc / (float) frame_size));
    }

    float in_rms = 0.0f;
    // Build the AEC input. ESP-SR multi-channel layout is planar:
    // [ch0 frame][ch1 frame]; mic 1 = slot 0 (MIC1), mic 2 = slot 2 (MIC2).
    for (size_t i = 0; i < (size_t) frame_size; ++i) {
      int16_t mic1 = rx_buf[i * RX_CHANNELS + CH_MIC1];
      int16_t mic2 = rx_buf[i * RX_CHANNELS + CH_MIC2];
      int16_t selected;
      switch (mic_mode) {
        case MIC_MODE_MIC2:
          selected = mic2;
          break;
        case MIC_MODE_MIX:
          selected = (int16_t) (((int32_t) mic1 + (int32_t) mic2) / 2);
          break;
        case MIC_MODE_MIC1:
        default:
          selected = mic1;  // dual modes: channel 0 = mic1
          break;
      }
      mic_buf[i] = selected;
      if (channels == 2) {
        mic_buf[frame_size + i] = mic2;
      }
      float mv = (float) selected;
      in_rms += mv * mv;
    }
    in_rms = sqrtf(in_rms / frame_size);

    uint32_t fill = 0;
    if (hw_ref) {
      // Analogue reference from the speaker amp (ES7210 MIC3). Already
      // delay-aligned by the hardware, so no offset/gain tweaking needed.
      for (size_t i = 0; i < (size_t) frame_size; ++i) {
        ref_buf[i] = rx_buf[i * RX_CHANNELS + CH_REF];
      }
    } else {
      fill = this->parent_->get_aec_ref_fill();
      uint32_t ref_offset = (uint32_t) this->aec_ref_offset_.load();
      if (fill < this->parent_->AEC_REF_SIZE && fill >= (uint32_t) frame_size) {
        this->parent_->pop_ref_samples(ref_buf, frame_size, ref_offset);
      } else if (fill < this->parent_->AEC_REF_SIZE && fill > 0) {
        this->parent_->pop_ref_samples(ref_buf, fill, ref_offset);
        std::fill(ref_buf + fill, ref_buf + frame_size, 0);
      } else {
        std::fill(ref_buf, ref_buf + frame_size, 0);
      }

      float ref_gain = this->aec_ref_gain_.load();
      if (ref_gain != 1.0f) {
        for (size_t i = 0; i < (size_t) frame_size; ++i) {
          float v = (float) ref_buf[i] * ref_gain;
          ref_buf[i] = (int16_t) std::max(-32768.0f, std::min(32767.0f, v));
        }
      }
    }

    aec_process(aec, mic_buf, ref_buf, out_buf);

    // Collapse the AEC output to mono (planar layout for the dual-mic case).
    for (size_t i = 0; i < (size_t) frame_size; ++i) {
      int32_t v;
      if (channels == 2) {
        int32_t o0 = out_buf[i];
        int32_t o1 = out_buf[frame_size + i];
        switch (mic_mode) {
          case MIC_MODE_DUAL_MIC1:
            v = o0;
            break;
          case MIC_MODE_DUAL_MIC2:
            v = o1;
            break;
          default:  // MIC_MODE_DUAL_AVG
            v = (o0 + o1) / 2;
            break;
        }
      } else {
        v = out_buf[i];
      }
      mono_buf[i] = (int16_t) std::max(-32768, std::min(32767, (int) v));
    }

    float out_rms = 0.0f;
    for (size_t i = 0; i < (size_t) frame_size; ++i) {
      float ov = (float) mono_buf[i];
      out_rms += ov * ov;
    }
    out_rms = sqrtf(out_rms / frame_size);
    float suppress_db = 0.0f;
    if (in_rms > 1e-3f && out_rms > 1e-3f)
      suppress_db = 20.0f * log10f(out_rms / in_rms);
    else if (in_rms > 1e-3f)
      suppress_db = -120.0f;
    this->aec_in_rms_.store(in_rms);
    this->aec_out_rms_.store(out_rms);
    this->aec_ref_fill_.store(fill);
    this->aec_suppress_db_.store(suppress_db);

    float gain = this->aec_gain_.load();
    for (size_t i = 0; i < (size_t) frame_size; ++i) {
      float v = (float) mono_buf[i] * gain;
      mono_buf[i] = (int16_t) std::max(-32768.0f, std::min(32767.0f, v));
    }

    aec_out.insert(aec_out.end(), mono_buf, mono_buf + frame_size);
    while (aec_out.size() >= 256) {
      if (this->active_listeners_.load() > 0) {
        std::vector<uint8_t> chunk(
            reinterpret_cast<const uint8_t *>(aec_out.data()),
            reinterpret_cast<const uint8_t *>(aec_out.data()) + (256 * sizeof(int16_t)));
        this->data_callbacks_.call(chunk);
      }

      aec_out.erase(aec_out.begin(), aec_out.begin() + 256);
    }

    if ((read_count % 50) == 0) {
      ESP_LOGD(MIC_TAG, "esp_aec #%" PRIu32 ": in=%.0f out=%.0f suppress=%.1f dB, ref=%s, ch=[%.0f %.0f %.0f %.0f], fill=%" PRIu32 ", offset=%d, buf=%zu",
               read_count, in_rms, out_rms, suppress_db,
               hw_ref ? "hw" : "sw",
               this->ch_rms_[0].load(), this->ch_rms_[1].load(),
               this->ch_rms_[2].load(), this->ch_rms_[3].load(),
               fill, this->aec_ref_offset_.load(), aec_out.size());
    }

    read_count++;
  }

  heap_caps_free(rx_buf);
  heap_caps_free(mic_buf);
  heap_caps_free(ref_buf);
  heap_caps_free(out_buf);
  heap_caps_free(mono_buf);
}

void WaveshareDuplexMicrophone::process_with_afe_() {
  // Re-assert the ES7210 ADC-clock/TDM setup: the es7210 component's setup()
  // can still run after the mic task's first attempt, and it re-gates ADC3/4.
  if (this->parent_ != nullptr) {
    this->tdm_state_.store(this->parent_->enable_es7210_tdm() ? 1 : -1);
  }

  ESP_LOGI(MIC_TAG, "AFE: starting (input format MR: mic1 slot0, hardware ref slot1)");

  // Models come from, in order of preference:
  //   1. a "model" flash partition (partitions.csv + srmodels.bin)
  //   2. the packed blob embedded in the app (scripts/pack-models.sh --emit-blob)
  //
  // The partition is inspected first: an erased or half-written partition reads
  // back as a model count of 0xFFFFFFFF, which esp-sr would happily turn into a
  // huge malloc, so refuse anything implausible and fall through to the blob.
  srmodel_list_t *models = nullptr;
  const esp_partition_t *model_part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
  if (model_part != nullptr) {
    uint32_t model_num = 0;
    const esp_err_t rd = esp_partition_read(model_part, 0, &model_num, sizeof(model_num));
    if (rd == ESP_OK && model_num >= 1 && model_num <= 64) {
      models = esp_srmodel_init("model");
      if (models != nullptr)
        ESP_LOGI(MIC_TAG, "AFE: loaded esp-sr models from the 'model' partition (%u models, %lu KiB)",
                 (unsigned) model_num, (unsigned long) (model_part->size / 1024));
    } else {
      ESP_LOGW(MIC_TAG, "AFE: 'model' partition present but empty/invalid (header=0x%08" PRIX32
                        ", err=%d) - ignoring it",
               model_num, (int) rd);
    }
  }
#ifdef USE_SRMODELS_BLOB
  if (models == nullptr) {
    models = srmodel_load((const void *) srmodels_blob);
    if (models != nullptr) {
      ESP_LOGI(MIC_TAG, "AFE: loaded embedded esp-sr models from app flash (%u KiB)",
               (unsigned) ((srmodels_blob_end - srmodels_blob) / 1024));
    }
  }
#endif
  const bool have_models = (models != nullptr);
  this->afe_have_models_.store(have_models ? 1 : 0);
  if (!have_models) {
    ESP_LOGW(MIC_TAG, "AFE: no esp-sr models (no 'model' partition, no embedded blob); "
                      "running model-less AEC (ns/wakenet off)");
  }

  const int requested_kind = this->afe_kind_.load();
  // AFE_TYPE_VC is unusable without an esp-sr "model" partition: NS is an
  // integral part of the VC pipeline, and without the NSNet model the VC
  // instance never yields a single fetch frame (verified on hardware).
  // FD is model-free and is the default the vendor firmware falls back to.
  const int kind = (requested_kind == AFE_KIND_VC) ? AFE_KIND_FD : requested_kind;
  if (kind != requested_kind) {
    ESP_LOGW(MIC_TAG, "AFE: 'vc' is unusable on esp-sr with MR input - using 'fd'");
  }
  this->afe_kind_eff_.store(kind);
  const bool hw_ref = this->ref_source_.load() == REF_SOURCE_HW;
  const afe_type_t afe_type = (kind == AFE_KIND_SR)   ? AFE_TYPE_SR
                              : (kind == AFE_KIND_VC) ? AFE_TYPE_VC
                                                      : AFE_TYPE_FD;
  // Input format "MR": one mic + one reference. With the on-board reference
  // (ES7210 MIC3 → TDM slot 1) this is exactly what the vendor firmware feeds.
  afe_config_t *cfg = afe_config_init("MR", models, afe_type, AFE_MODE_HIGH_PERF);
  if (cfg == nullptr) {
    ESP_LOGE(MIC_TAG, "AFE: afe_config_init failed - falling back to esp_aec");
    if (models != nullptr) esp_srmodel_deinit(models);
    this->audio_frontend_.store(FRONTEND_AEC);
    return;
  }
  // What esp-sr chose by itself for this input format/type (log before we
  // touch anything: the per-type defaults are not documented).
  ESP_LOGI(MIC_TAG, "AFE defaults (esp-sr): aec=%d se=%d ns=%d vad=%d wakenet=%d agc=%d ns_mode=%d",
           (int) cfg->aec_init, (int) cfg->se_init, (int) cfg->ns_init, (int) cfg->vad_init,
           (int) cfg->wakenet_init, (int) cfg->agc_init, (int) cfg->afe_ns_mode);
  const bool sr_defaults_se = cfg->se_init;

  cfg->aec_init = true;
  // SR mode has no nonlinear echo suppression; FD is the full-duplex scenario
  // (playback + mic at the same time); VC is what the vendor firmware uses.
  cfg->aec_mode = (kind == AFE_KIND_SR)   ? AEC_MODE_SR_HIGH_PERF
                  : (kind == AFE_KIND_VC) ? AEC_MODE_VOIP_HIGH_PERF
                                          : AEC_MODE_FD_HIGH_PERF;
  cfg->aec_nlp_level = (aec_nlp_level_t) this->aec_nlp_target_.load();
  // Single mic + hardware reference: there is no mic array to beam-form, so SE
  // stays off — except for VC, where turning it off made esp-sr crash with a
  // NULL store during create_from_config (observed on device), so keep
  // whatever default esp-sr picked for VC.
  cfg->se_init = (kind == AFE_KIND_VC) ? sr_defaults_se : false;
  cfg->ns_init = have_models && this->afe_ns_.load();   // NSNet needs a model file
  // The AFE's own VAD stays off: with vadnet1_medium it starves the main loop
  // so badly the API stops answering (verified on 2.4.3 and 2.5.3). Barge-in
  // uses the free energy detector on the AEC output instead.
  cfg->vad_init = false;
  cfg->wakenet_init = false;    // micro_wake_word does its own wake/stop word
  // Vendor firmware runs AEC-only (no AGC); AGC is a wake-word-level crutch
  // because micro_wake_word consumes the AFE output here (xiaozhi feeds its
  // wake word the raw mic instead).
  cfg->agc_init = this->afe_agc_.load();
  cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

  const esp_afe_sr_iface_t *iface = esp_afe_handle_from_config(cfg);
  esp_afe_sr_data_t *afe = (iface != nullptr) ? iface->create_from_config(cfg) : nullptr;
  if (afe == nullptr) {
    ESP_LOGE(MIC_TAG, "AFE: create_from_config failed - falling back to esp_aec");
    afe_config_free(cfg);
    if (models != nullptr) esp_srmodel_deinit(models);
    this->audio_frontend_.store(FRONTEND_AEC);
    return;
  }

  const int feed_n = iface->get_feed_chunksize(afe);
  const int ch_num = iface->get_channel_num(afe);
  const int fetch_n = iface->get_fetch_chunksize(afe);
  this->afe_feed_n_.store(feed_n);
  this->afe_fetch_n_.store(fetch_n);
  this->afe_ch_num_.store(ch_num);
  ESP_LOGI(MIC_TAG, "AFE ready: kind=%s, feed=%d/ch, ch_num=%d, fetch=%d, aec_mode=%d, nlp=%d, ref=%s, "
                    "agc=%s, ns=%s, se=%s, models=%s",
           afe_kind_name(kind), feed_n, ch_num, fetch_n, (int) cfg->aec_mode,
           (int) cfg->aec_nlp_level, hw_ref ? "hw" : "sw",
           cfg->agc_init ? "on" : "off", cfg->ns_init ? "on" : "off",
           cfg->se_init ? "on" : "off", have_models ? "yes" : "no");
  iface->print_pipeline(afe);

  const uint32_t ratio = this->parent_->get_decimation_ratio();
  const size_t read_frames = (size_t) feed_n * (size_t) ratio;
  const size_t rx_bytes = read_frames * RX_CHANNELS * sizeof(int16_t);
  int16_t *rx_buf = (int16_t *) heap_caps_aligned_alloc(16, rx_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  int16_t *feed_buf = (int16_t *) heap_caps_aligned_alloc(
      16, (size_t) feed_n * (size_t) ch_num * sizeof(int16_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  int16_t *ref_buf = (int16_t *) heap_caps_aligned_alloc(16, (size_t) feed_n * sizeof(int16_t),
                                                         MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (rx_buf == nullptr || feed_buf == nullptr || ref_buf == nullptr) {
    ESP_LOGE(MIC_TAG, "AFE: buffer allocation failed - falling back to esp_aec");
    if (rx_buf) heap_caps_free(rx_buf);
    if (feed_buf) heap_caps_free(feed_buf);
    if (ref_buf) heap_caps_free(ref_buf);
    iface->destroy(afe);
    afe_config_free(cfg);
    if (models != nullptr) esp_srmodel_deinit(models);
    this->audio_frontend_.store(FRONTEND_AEC);
    return;
  }

  std::vector<int16_t> out_ring;
  const uint32_t cfg_gen = this->aec_config_gen_.load();
  uint32_t read_count = 0;
  uint32_t feeds_done = 0;

  while (this->audio_frontend_.load() == FRONTEND_AFE && this->aec_config_gen_.load() == cfg_gen) {
    size_t bytes_read = 0;
    i2s_channel_read(this->parent_->get_rx_handle(), rx_buf, rx_bytes, &bytes_read, pdMS_TO_TICKS(200));
    if (bytes_read == 0) {
      this->rx_empty_.fetch_add(1);
      continue;
    }
    this->rx_frames_.fetch_add(1);

    // Bus rate → voice rate (no-op on a 16 kHz bus).
    decimate_tdm_in_place(rx_buf, read_frames, (size_t) feed_n, ratio);

    // Raw software reference: the AFE estimates the delay itself.
    if (hw_ref) {
      for (int i = 0; i < feed_n; ++i)
        ref_buf[i] = rx_buf[(size_t) i * RX_CHANNELS + CH_REF];
    } else {
      uint32_t fill = this->parent_->get_aec_ref_fill();
      if (fill >= (uint32_t) feed_n) {
        this->parent_->pop_ref_samples(ref_buf, feed_n, 0);
      } else if (fill > 0) {
        this->parent_->pop_ref_samples(ref_buf, fill, 0);
        std::fill(ref_buf + fill, ref_buf + feed_n, 0);
      } else {
        std::fill(ref_buf, ref_buf + feed_n, 0);
      }
    }

    float in_rms = 0.0f;
    float ch_acc[RX_CHANNELS] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < feed_n; ++i) {
      for (uint32_t c = 0; c < RX_CHANNELS; ++c) {
        float cv = (float) rx_buf[(size_t) i * RX_CHANNELS + c];
        ch_acc[c] += cv * cv;
      }
      int16_t mic1 = rx_buf[(size_t) i * RX_CHANNELS + CH_MIC1];
      int16_t *dst = feed_buf + (size_t) i * (size_t) ch_num;
      dst[0] = mic1;
      if (ch_num > 1) dst[1] = ref_buf[i];
      for (int c = 2; c < ch_num; ++c) dst[c] = 0;
      float mv = (float) mic1;
      in_rms += mv * mv;
    }
    in_rms = sqrtf(in_rms / feed_n);
    for (uint32_t c = 0; c < RX_CHANNELS; ++c)
      this->ch_rms_[c].store(sqrtf(ch_acc[c] / feed_n));

    iface->feed(afe, feed_buf);
    feeds_done++;

    // Feed and fetch sizes are independent: with the "MR" layout this AFE runs
    // feed=256 samples/ch but fetch=512 samples/ch, i.e. one output frame needs
    // two feeds. So never block for output after a single feed — keep feeding
    // and poll. (Blocking here deadlocked the first fetch forever, which is why
    // the AFE looked dead at boot.)
    afe_fetch_result_t *res = iface->fetch_with_delay(afe, 0);
    const size_t out_start = out_ring.size();
    int produced = 0;
    const float gain = this->aec_gain_.load();
    if (res != nullptr && res->data != nullptr && res->data_size > 0) {
      const int n = res->data_size / (int) sizeof(int16_t);
      for (int i = 0; i < n; ++i) {
        float v = (float) res->data[i] * gain;
        out_ring.push_back((int16_t) std::max(-32768.0f, std::min(32767.0f, v)));
      }
      produced = n;
    }

    if (produced == 0) {
      this->afe_fetch_empty_.fetch_add(1);
      // No output for a long stretch (≈5 s of continuous feeding) means this
      // AFE instance is broken: recreate it, then give up and use the plain
      // esp_aec front-end rather than staying deaf. The first frames after
      // creation legitimately produce nothing while the pipeline fills up.
      if (feeds_done > 300) {
        if (this->afe_retries_.load() < 3) {
          this->afe_retries_.fetch_add(1);
          ESP_LOGE(MIC_TAG, "AFE: no output after %" PRIu32 " feeds (retry %d/3) - recreating",
                   feeds_done, this->afe_retries_.load());
        } else {
          ESP_LOGE(MIC_TAG, "AFE: still no output after %d retries (models=%d) - falling back to esp_aec",
                   this->afe_retries_.load(), this->afe_have_models_.load(),
           this->active_listeners_.load(),
           this->speech_events_.load());
          this->audio_frontend_.store(FRONTEND_AEC);
        }
        break;  // destroy + return; mic_task recreates or uses the aec path
      }
      continue;
    }
    this->afe_produced_.fetch_add(produced);

    float out_rms = 0.0f;
    for (size_t i = out_start; i < out_ring.size(); ++i) {
      float ov = (float) out_ring[i];
      out_rms += ov * ov;
    }
    out_rms = sqrtf(out_rms / (float) (out_ring.size() - out_start));
    float suppress_db = 0.0f;
    if (in_rms > 1e-3f && out_rms > 1e-3f)
      suppress_db = 20.0f * log10f(out_rms / in_rms);
    else if (in_rms > 1e-3f)
      suppress_db = -120.0f;
    this->aec_in_rms_.store(in_rms);
    this->aec_out_rms_.store(out_rms);
    this->aec_suppress_db_.store(suppress_db);

    // Energy-based barge-in on the echo-cancelled output (rising edges only).
    if (out_rms >= this->barge_in_level_.load()) {
      if (!this->barge_active_) {
        this->barge_active_ = true;
        const uint32_t n = this->speech_events_.fetch_add(1) + 1;
        ESP_LOGD(MIC_TAG, "barge-in onset #%" PRIu32 " (out=%.0f)", n, out_rms);
      }
    } else if (this->barge_active_ && out_rms < this->barge_in_level_.load() * 0.6f) {
      this->barge_active_ = false;  // hysteresis so one utterance counts once
    }
    this->aec_ref_fill_.store(hw_ref ? 0 : this->parent_->get_aec_ref_fill());

    while (out_ring.size() >= 256) {
      if (this->active_listeners_.load() > 0) {
        std::vector<uint8_t> chunk(
            reinterpret_cast<const uint8_t *>(out_ring.data()),
            reinterpret_cast<const uint8_t *>(out_ring.data()) + (256 * sizeof(int16_t)));
        this->data_callbacks_.call(chunk);
      }
      out_ring.erase(out_ring.begin(), out_ring.begin() + 256);
    }

    if ((read_count % 50) == 0) {
      ESP_LOGD(MIC_TAG, "AFE #%" PRIu32 ": in=%.0f out=%.0f suppress=%.1f dB ref=%s ch=[%.0f %.0f %.0f %.0f] produced=%d/%d",
               read_count, in_rms, out_rms, suppress_db, hw_ref ? "hw" : "sw",
               sqrtf(ch_acc[0] / feed_n), sqrtf(ch_acc[1] / feed_n),
               sqrtf(ch_acc[2] / feed_n), sqrtf(ch_acc[3] / feed_n), produced, feed_n);
    }
    read_count++;
  }

  heap_caps_free(rx_buf);
  heap_caps_free(feed_buf);
  heap_caps_free(ref_buf);
  iface->destroy(afe);
  afe_config_free(cfg);
  if (models != nullptr) esp_srmodel_deinit(models);
  ESP_LOGI(MIC_TAG, "AFE: stopped");
}



[[noreturn]] void WaveshareDuplexMicrophone::mic_task(void *param) {
  auto *self = (WaveshareDuplexMicrophone *) param;

  ESP_LOGI(MIC_TAG, "mic_task: started (frontend=%s)", frontend_name(self->get_audio_frontend()));

  // ESPHome's es7210 component always configures I2S (2-slot) mode in setup().
  // Re-enable TDM 4-slot here — after every component's setup() has run — so
  // the RX stream matches the TDM layout the parent initialised. Without this
  // the ADC only sends MIC1/MIC2 and the hardware echo reference is missing.
  if (self->parent_ != nullptr) {
    self->tdm_state_.store(self->parent_->enable_es7210_tdm() ? 1 : -1);
    if (self->tdm_state_.load() < 0)
      ESP_LOGE(MIC_TAG, "mic_task: ES7210 TDM enable failed — mic audio will be wrong");
  }

  while (true) {
    if (self->get_audio_frontend() == FRONTEND_AFE) {
      self->process_with_afe_();
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Plain esp_aec front-end: used when "Audio Frontend" is aec, or as the
    // automatic fallback if the AFE fails to initialise.
    const int mic_mode = self->get_mic_mode();
    const bool dual = mic_mode_is_dual(mic_mode);
    aec_config_t config = {
        .mic_num       = dual ? 2 : 1,
        .ref_num       = 1,
        .out_num       = dual ? 2 : 1,
        .filter_length = self->aec_filter_length_.load(),
        .sample_rate   = 16000,
        .caps          = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        .mode          = (aec_mode_t) self->aec_mode_.load(),
        .nlp_level     = (aec_nlp_level_t) self->aec_nlp_target_.load(),
    };
    aec_handle_t *aec = aec_create_from_config(&config);
    if (!aec) {
      ESP_LOGE(MIC_TAG, "Failed to create esp_aec handle (mode=%d, filter=%d)",
               (int) config.mode, config.filter_length);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;  // Retry
    }

    int frame_size = aec_get_chunksize(aec);
    ESP_LOGI(MIC_TAG, "esp_aec initialized (frame=%d, mode=%d, filter=%d, gain=%.1f, nlp=%d)",
             frame_size, (int) config.mode, config.filter_length, self->aec_gain_.load(),
             self->aec_nlp_target_.load());

    self->process_with_nlms_(aec, frame_size);

    aec_destroy(aec);
    ESP_LOGI(MIC_TAG, "esp_aec: processing ended (config change)");
    // The processing loop ended (config change) - wait a moment and re-enter.
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  vTaskDelete(NULL);
}

}  // namespace waveshare_duplex_audio
}  // namespace esphome

#endif  // USE_ESP32
