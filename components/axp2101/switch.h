#pragma once

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "axp2101.h"

namespace esphome {
namespace axp2101 {

class AXP2101Component;

// Exposes the AXP2101 cell-battery charge enable bit (reg 0x18 bit1, CHG_EN)
// as a Home Assistant switch, mirroring the CoreS3 "usb_otg_en" switch used by
// the battery range manager automation.
class AXP2101ChargingSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(AXP2101Component *parent) { this->parent_ = parent; }
  void setup() override;
  void dump_config() override;

 protected:
  void write_state(bool state) override;
  AXP2101Component *parent_{nullptr};
};

}  // namespace axp2101
}  // namespace esphome
