#include "switch.h"
#include "esphome/core/log.h"

namespace esphome {
namespace axp2101 {

static const char *const TAG = "axp2101.switch";

void AXP2101ChargingSwitch::setup() {
  // Restore the last commanded state (default ON) so a reboot does not
  // re-enable charging while the battery is already above the upper bound.
  auto initial = this->get_initial_state_with_restore_mode();
  bool state;
  if (initial.has_value()) {
    state = *initial;
  } else {
    // restore_mode was set to DISABLED: fall back to the live PMU state.
    state = this->parent_ != nullptr && this->parent_->is_charging_enabled();
  }

  ESP_LOGD(TAG, "Charging switch initial state: %s", ONOFF(state));
  if (state) {
    this->turn_on();
  } else {
    this->turn_off();
  }
}

void AXP2101ChargingSwitch::write_state(bool state) {
  if (this->parent_ == nullptr) {
    ESP_LOGW(TAG, "No AXP2101 parent, ignoring charging state write");
    return;
  }
  // Safety floor: refuse to disable charging when the battery is at or below
  // the cutoff, so the device can never be left to drain to 0% uncharged.
  if (!state && !this->parent_->charging_disable_allowed()) {
    float level = this->parent_->get_battery_level();
    ESP_LOGW(TAG, "Refusing to disable charging: battery at %.0f%% (<= %.0f%%)",
             level, this->parent_->get_min_charging_level());
    this->publish_state(true);  // keep/return to charging enabled
    return;
  }
  if (this->parent_->set_charging_enabled(state)) {
    this->publish_state(state);
  } else {
    ESP_LOGW(TAG, "Failed to write charging state %s", ONOFF(state));
  }
}

void AXP2101ChargingSwitch::dump_config() { LOG_SWITCH("", "AXP2101 Charging Switch", this); }

}  // namespace axp2101
}  // namespace esphome
