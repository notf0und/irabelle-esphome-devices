#include "stream_microphone.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace stream_microphone {

static const char *const TAG = "stream_microphone";

void RestoreText::control(const std::string &value) {
  this->publish_state(value);
  if (parent_ != nullptr)
    parent_->on_text_changed_();
}

void StreamMicrophone::save_nvs_(const std::string &key, const std::string &value) {
  uint32_t hash = fnv1_hash(key);
  auto pref = global_preferences->make_preference<uint8_t[256]>(hash);
  size_t len = std::min(value.size(), (size_t) 255);
  uint8_t buf[256];
  buf[0] = static_cast<uint8_t>(len);
  memcpy(buf + 1, value.data(), len);
  pref.save(&buf);
}

void StreamMicrophone::restore_nvs_() {
  ip_pref_ = global_preferences->make_preference<uint8_t[256]>(fnv1_hash("stream_mic_ip"));
  port_pref_ = global_preferences->make_preference<uint8_t[256]>(fnv1_hash("stream_mic_port"));

  auto restore_text = [this](RestoreText *text, const std::string &nvs_key) {
    if (text == nullptr)
      return;
    std::string value = text->get_initial_value();
    uint8_t buf[256];
    auto pref = global_preferences->make_preference<uint8_t[256]>(fnv1_hash(nvs_key));
    if (pref.load(&buf)) {
      size_t len = buf[0];
      if (len > 0 && len <= 255)
        value = std::string(reinterpret_cast<char *>(buf + 1), len);
    }
    if (!value.empty())
      text->publish_state(value);
  };

  restore_text(ip_text_, "stream_mic_ip");
  restore_text(port_text_, "stream_mic_port");
  restore_text(gain_text_, "stream_mic_gain");
}

void StreamMicrophone::on_text_changed_() {
  if (ip_text_ != nullptr)
    save_nvs_("stream_mic_ip", ip_text_->state);
  if (port_text_ != nullptr)
    save_nvs_("stream_mic_port", port_text_->state);
  if (gain_text_ != nullptr)
    save_nvs_("stream_mic_gain", gain_text_->state);

  if (ip_text_ != nullptr && port_text_ != nullptr && !port_text_->state.empty()) {
    char *end = nullptr;
    long p = strtol(port_text_->state.c_str(), &end, 10);
    set_target(
        ip_text_->state,
        end != port_text_->state.c_str() ? static_cast<uint16_t>(p) : target_port_);
  }
  if (gain_text_ != nullptr && !gain_text_->state.empty()) {
    char *end = nullptr;
    long g = strtol(gain_text_->state.c_str(), &end, 10);
    if (end != gain_text_->state.c_str() && g >= 1 && g <= 64)
      gain_factor_ = static_cast<uint8_t>(g);
    ESP_LOGD(TAG, "Gain factor set to %u", gain_factor_);
  }
}

void StreamMicrophone::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Stream Microphone...");

  if (!setup_socket_()) {
    mark_failed();
    return;
  }

  restore_nvs_();

  if (ip_text_ != nullptr)
    target_ip_ = ip_text_->state;
  if (port_text_ != nullptr && !port_text_->state.empty()) {
    char *end = nullptr;
    long p = strtol(port_text_->state.c_str(), &end, 10);
    if (end != port_text_->state.c_str() && p >= 1 && p <= 65535)
      target_port_ = static_cast<uint16_t>(p);
  }
  if (gain_text_ != nullptr && !gain_text_->state.empty()) {
    char *end = nullptr;
    long g = strtol(gain_text_->state.c_str(), &end, 10);
    if (end != gain_text_->state.c_str() && g >= 1 && g <= 64)
      gain_factor_ = static_cast<uint8_t>(g);
  }

  set_target(target_ip_, target_port_);
  ESP_LOGI(TAG, "Initial gain factor: %u", gain_factor_);
}

void StreamMicrophone::dump_config() {
  ESP_LOGCONFIG(TAG, "Stream Microphone:");
  ESP_LOGCONFIG(TAG, "  Target: %s:%u", target_ip_.c_str(), target_port_);
  ESP_LOGCONFIG(TAG, "  Gain factor: %u", gain_factor_);
  LOG_TEXT("  ", "IP Text", ip_text_);
  LOG_TEXT("  ", "Port Text", port_text_);
  LOG_TEXT("  ", "Gain Text", gain_text_);
}

bool StreamMicrophone::setup_socket_() {
  socket_ = socket::socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (socket_ == nullptr) {
    ESP_LOGE(TAG, "Could not create UDP socket");
    return false;
  }
  int enable = 1;
  int err = socket_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
  if (err != 0)
    ESP_LOGW(TAG, "Socket unable to set reuseaddr: errno %d", err);
  err = socket_->setblocking(false);
  if (err != 0) {
    ESP_LOGE(TAG, "Socket unable to set nonblocking: errno %d", err);
    return false;
  }
  return true;
}

void StreamMicrophone::set_target(const std::string &ip, uint16_t port) {
  target_ip_ = ip;
  target_port_ = port;
  auto addr_len = socket::set_sockaddr(
      reinterpret_cast<struct sockaddr *>(&dest_addr_),
      sizeof(dest_addr_),
      ip.c_str(), port);
  if (addr_len > 0)
    ESP_LOGD(TAG, "Target set to %s:%u", ip.c_str(), port);
  else
    ESP_LOGE(TAG, "Failed to resolve target address: %s", ip.c_str());
}

void StreamMicrophone::send(std::vector<uint8_t> data) {
  if (socket_ == nullptr)
    return;
  if (gain_factor_ > 1 && data.size() >= 2) {
    int16_t *samples = reinterpret_cast<int16_t *>(data.data());
    size_t count = data.size() / 2;
    for (size_t i = 0; i < count; i++) {
      int32_t scaled = samples[i] * gain_factor_;
      samples[i] = clamp<int32_t>(scaled, -32768, 32767);
    }
  }
  int sent = socket_->sendto(
      data.data(), data.size(), 0,
      reinterpret_cast<const struct sockaddr *>(&dest_addr_),
      sizeof(dest_addr_));
  if (sent < 0)
    ESP_LOGW(TAG, "UDP sendto failed (errno=%d)", errno);
}

}  // namespace stream_microphone
}  // namespace esphome
