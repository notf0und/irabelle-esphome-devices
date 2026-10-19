#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/components/text/text.h"
#include "esphome/components/socket/socket.h"
#include "esphome/components/network/ip_address.h"

namespace esphome {
namespace stream_microphone {

class StreamMicrophone;

class RestoreText : public text::Text {
 public:
  void set_initial_value(const std::string &value) { initial_value_ = value; }
  void set_parent(StreamMicrophone *parent) { parent_ = parent; }
  const std::string &get_initial_value() const { return initial_value_; }

 protected:
  void control(const std::string &value) override;
  std::string initial_value_;
  StreamMicrophone *parent_{nullptr};
};

class StreamMicrophone : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void setup() override;
  void dump_config() override;

  void set_ip_text(RestoreText *t) { ip_text_ = t; }
  void set_port_text(RestoreText *t) { port_text_ = t; }
  void set_gain_text(RestoreText *t) { gain_text_ = t; }

  void send(std::vector<uint8_t> data);
  void set_target(const std::string &ip, uint16_t port);
  void on_text_changed_();

  RestoreText *ip_text_{nullptr};
  RestoreText *port_text_{nullptr};
  RestoreText *gain_text_{nullptr};

 protected:
  bool setup_socket_();
  void restore_nvs_();
  void save_nvs_(const std::string &key, const std::string &value);

  std::string target_ip_{"192.168.1.2"};
  uint16_t target_port_{5005};
  uint8_t gain_factor_{1};

  struct sockaddr_storage dest_addr_{};
  std::unique_ptr<socket::Socket> socket_{nullptr};

  ESPPreferenceObject ip_pref_;
  ESPPreferenceObject port_pref_;
};

}  // namespace stream_microphone
}  // namespace esphome
