#pragma once

#ifdef USE_ESP32

#include "../waveshare_duplex_audio.h"

#include "esphome/components/microphone/microphone.h"
#include "esphome/core/component.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstring>
#include <cmath>
#include <string>
#include <esp_aec.h>

namespace esphome {
namespace waveshare_duplex_audio {

/**
 * Duplex microphone with standalone ESP-SR AEC (Acoustic Echo Cancellation).
 *
 * Uses aec_create + aec_process from ESP-SR v2.4.3+ (no model files needed).
 * aec_gain and aec_nlp can be tuned at runtime via Home Assistant entities.
 */
class WaveshareDuplexMicrophone : public microphone::Microphone, public Component {
 public:
  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() override;
  void dump_config() override;
  void loop() override;

  void start() override;
  void stop() override;

  void set_parent(WaveshareDuplexAudio *parent) { this->parent_ = parent; }
  // Rate this microphone publishes to the voice stack. Must stay at
  // WaveshareDuplexAudio::VOICE_SAMPLE_RATE (16 kHz) for the ESP-SR AFE and
  // micro_wake_word; the I2S bus may run faster and is decimated to match.
  void set_output_sample_rate(uint32_t rate) { this->output_sample_rate_ = rate; }
  void set_aec_gain(float gain) { this->aec_gain_.store(gain); }
  void set_aec_nlp(int nlp);

  float get_aec_gain() const { return this->aec_gain_.load(); }
  int get_aec_nlp() const { return this->aec_nlp_target_.load(); }

  void set_aec_mode(int mode);
  void set_aec_filter_length(int length);
  void set_aec_ref_offset(int offset);
  void set_aec_ref_gain(float gain);

  int get_aec_mode() const { return this->aec_mode_.load(); }
  int get_aec_filter_length() const { return this->aec_filter_length_.load(); }
  int get_aec_ref_offset() const { return this->aec_ref_offset_.load(); }
  float get_aec_ref_gain() const { return this->aec_ref_gain_.load(); }

  float get_aec_in_rms() const { return this->aec_in_rms_.load(); }
  float get_aec_out_rms() const { return this->aec_out_rms_.load(); }
  float get_aec_suppress_db() const { return this->aec_suppress_db_.load(); }
  uint32_t get_aec_ref_fill() const { return this->aec_ref_fill_.load(); }

  // Per-RX-slot RMS of the raw ES7210 TDM frame (diagnostics). Slot order is
  // MIC1, MIC3(ref), MIC2, MIC4 — see WaveshareDuplexAudio::RX_SLOT_*.
  // How many consumers (micro_wake_word, voice assistant, stream_microphone)
  // are currently subscribed to the processed stream.
  int get_active_listeners() const { return this->active_listeners_.load(); }

  float get_ch_rms(int slot) const {
    return (slot >= 0 && slot < WaveshareDuplexAudio::RX_CHANNELS) ? this->ch_rms_[slot].load() : 0.0f;
  }

  // RX plumbing diagnostics. Frames counts successful I2S reads, empty counts
  // reads that returned no data (the loop uses a bounded timeout so a dead RX
  // cannot wedge the mic task). tdm is -1 when the ES7210 TDM switch failed,
  // 0 before the first attempt, 1 when the ADC is in TDM 4-slot mode.
  uint32_t get_rx_frames() const { return this->rx_frames_.load(); }
  uint32_t get_rx_empty() const { return this->rx_empty_.load(); }
  int get_tdm_state() const { return this->tdm_state_.load(); }
  uint32_t get_afe_produced() const { return this->afe_produced_.load(); }
  int get_afe_retries() const { return this->afe_retries_.load(); }
  int get_afe_feed_n() const { return this->afe_feed_n_.load(); }
  int get_afe_ch_num() const { return this->afe_ch_num_.load(); }
  static const char *ch_slot_name(int slot) {
    switch (slot) {
      case WaveshareDuplexAudio::RX_SLOT_MIC1: return "mic1(MIC1)";
      case WaveshareDuplexAudio::RX_SLOT_REF:  return "ref(MIC3)";
      case WaveshareDuplexAudio::RX_SLOT_MIC2: return "mic2(MIC2)";
      default: return "aux(MIC4)";
    }
  }

  // AEC reference source. "hardware" uses the on-board analogue echo reference
  // (ES7210 MIC3, fed from the speaker amp through the AEC divider) — the same
  // reference the vendor firmware uses. "software" falls back to the playback
  // ring buffer copy pushed by the speaker task, which needs manual
  // delay/gain alignment (the AEC Ref Offset/Gain numbers).
  enum RefSource {
    REF_SOURCE_HW = 0,
    REF_SOURCE_SOFTWARE = 1,
  };
  static const char *ref_source_name(int mode) {
    return mode == REF_SOURCE_SOFTWARE ? "software" : "hardware";
  }
  void set_ref_source(int mode);
  int get_ref_source() const { return this->ref_source_.load(); }

  // AFE stage toggles (take effect on the next AFE (re)creation):
  //   ns  = NSNet neural noise suppression (needs models)
  //   agc = AFE automatic gain control, targets about -3 dBFS (esp-sr default);
  //         with AGC on keep the post "AEC Gain" near 1.0 or the output clips.
  void set_afe_ns(bool enabled);
  void set_afe_agc(bool enabled);
  bool get_afe_ns() const { return this->afe_ns_.load(); }
  bool get_afe_agc() const { return this->afe_agc_.load(); }

  // Energy-based barge-in. The AFE output is the echo-cancelled signal, so while
  // the speaker plays the residual sits at ~0-10 RMS and any real speech jumps to
  // hundreds. We already compute that level every frame, so counting rising edges
  // is free (no VAD model, no extra CPU). get_speech_events() counts them and the
  // YAML turns them into "stop the announcement". Level is tunable because it is
  // measured AFTER the "AEC Gain" post-gain.
  void set_barge_in_level(float level);
  float get_barge_in_level() const { return this->barge_in_level_.load(); }
  uint32_t get_speech_events() const { return this->speech_events_.load(); }

  // Dual-mic AEC staging. RX slot map (ES7210 TDM order, see parent):
  //   slot 0 = MIC1 (board mic), slot 2 = MIC2 (second board mic),
  //   slot 1 = MIC3 (hardware echo reference — never used as a mic).
  // Runtime-switchable so the single-mic behaviour stays available for A/B.
  enum MicMode {
    MIC_MODE_MIC1 = 0,        // single mic, slot 0 (legacy default)
    MIC_MODE_MIC2 = 1,        // single mic, slot 2
    MIC_MODE_MIX = 2,         // sum both mics pre-AEC (poor-man's beamformer)
    MIC_MODE_DUAL_MIC1 = 3,   // 2-mic AEC, use output channel 0
    MIC_MODE_DUAL_MIC2 = 4,   // 2-mic AEC, use output channel 1
    MIC_MODE_DUAL_AVG = 5,    // 2-mic AEC, average both outputs
  };
  static bool mic_mode_is_dual(int mode) { return mode >= MIC_MODE_DUAL_MIC1; }
  void set_mic_mode(int mode);
  int get_mic_mode() const { return this->mic_mode_.load(); }

  // Audio front-end selector: standalone esp_aec (default) or the full ESP-SR
  // AFE (AEC + multi-mic SE + NS + AGC). AFE needs an esp-sr "model" flash
  // partition; without one it logs and falls back to esp_aec automatically.
  enum Frontend {
    FRONTEND_AEC = 0,
    FRONTEND_AFE = 1,
  };
  static const char *frontend_name(int mode) { return mode == FRONTEND_AFE ? "afe" : "aec"; }
  void set_audio_frontend(int mode);
  int get_audio_frontend() const { return this->audio_frontend_.load(); }

  // AFE scenario type: SR (no nonlinear echo suppression) / FD (full duplex,
  // recommended while the speaker is playing) / VC (voice comms).
  enum AfeKind {
    AFE_KIND_SR = 0,
    AFE_KIND_FD = 1,
    AFE_KIND_VC = 2,
  };
  static const char *afe_kind_name(int kind) {
    switch (kind) {
      case AFE_KIND_SR: return "sr";
      case AFE_KIND_VC: return "vc";
      default: return "fd";
    }
  }
  void set_afe_kind(int kind);
  int get_afe_kind() const { return this->afe_kind_.load(); }
  // AFE type actually in use (differs from get_afe_kind() when 'vc' was
  // requested without models and the firmware substituted 'fd').
  int get_afe_kind_effective() const { return this->afe_kind_eff_.load(); }
  // 1 when ESP-SR models (NSNet/VADNet) were found and loaded.
  int get_afe_have_models() const { return this->afe_have_models_.load(); }

 protected:
    [[noreturn]] static void mic_task(void *params);

    // NLMS processing method (takes aec handle and frame size, runs until algorithm switch)
    void process_with_nlms_(aec_handle_t *aec, int frame_size);

    // ESP-SR AFE processing (runs until frontend/config changes)
    void process_with_afe_();

    WaveshareDuplexAudio *parent_{nullptr};
    uint32_t output_sample_rate_{WaveshareDuplexAudio::VOICE_SAMPLE_RATE};
    TaskHandle_t task_handle_{nullptr};

    std::atomic<int> active_listeners_{0};
    std::atomic<float> aec_gain_{8.0f};
    std::atomic<int> aec_nlp_target_{1};  // AEC_NLP_LEVEL_AGGR

    std::atomic<int> aec_mode_{AEC_MODE_FD_HIGH_PERF};
    std::atomic<int> aec_filter_length_{4};
    std::atomic<int> aec_ref_offset_{0};
    std::atomic<float> aec_ref_gain_{2.0f};
    std::atomic<int> mic_mode_{MIC_MODE_MIC1};
    std::atomic<int> ref_source_{REF_SOURCE_HW};
    std::atomic<int> audio_frontend_{FRONTEND_AFE};
    std::atomic<int> afe_kind_{AFE_KIND_FD};
    std::atomic<bool> afe_ns_{true};
    std::atomic<bool> afe_agc_{false};
    std::atomic<float> barge_in_level_{200.0f};
    std::atomic<uint32_t> speech_events_{0};
    bool barge_active_{false};
    std::atomic<int> afe_kind_eff_{AFE_KIND_FD};
    std::atomic<uint32_t> aec_config_gen_{0};

    std::atomic<float> aec_in_rms_{0.0f};
    std::atomic<float> aec_out_rms_{0.0f};
    std::atomic<float> aec_suppress_db_{0.0f};
    std::atomic<uint32_t> aec_ref_fill_{0};
    std::atomic<float> ch_rms_[WaveshareDuplexAudio::RX_CHANNELS]{};
    std::atomic<uint32_t> rx_frames_{0};
    std::atomic<uint32_t> rx_empty_{0};
    std::atomic<int> tdm_state_{0};
    // AFE plumbing diagnostics.
    std::atomic<int> afe_feed_n_{0};
    std::atomic<int> afe_fetch_n_{0};
    std::atomic<int> afe_ch_num_{0};
    std::atomic<int> afe_have_models_{0};
    std::atomic<uint32_t> afe_produced_{0};
    std::atomic<uint32_t> afe_fetch_empty_{0};
    std::atomic<int> afe_retries_{0};
    bool afe_settled_{false};
    uint32_t status_logs_{0};

};

}  // namespace waveshare_duplex_audio
}  // namespace esphome

#endif  // USE_ESP32
