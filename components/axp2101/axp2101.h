//axp2101.h
#ifndef __AXP2101_H__
#define __AXP2101_H__

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "driver/gpio.h"

// Forward declaration
namespace esphome {
namespace number {
class Number;
}
}

namespace esphome {
namespace switch_ {
class Switch;
}
namespace axp2101 {

// AXP2101 Register Definitions
#define AXP2101_I2C_ADDR 0x34

// Power control registers
#define AXP2101_COMM_CFG 0x00
#define AXP2101_COMM_STAT0 0x01
#define AXP2101_COMM_STAT1 0x02
#define AXP2101_CHIP_ID 0x03

// Power output control
#define AXP2101_DC_WORK_MODE 0x80
#define AXP2101_DC_VOL0 0x82
#define AXP2101_DC_VOL1 0x83
#define AXP2101_DC_VOL2 0x84
#define AXP2101_DC_VOL3 0x85
#define AXP2101_DC_VOL4 0x86

// LDO control
#define AXP2101_ALDO_VOL0 0x92
#define AXP2101_ALDO_VOL1 0x93
#define AXP2101_ALDO_VOL2 0x94
#define AXP2101_ALDO_VOL3 0x95
#define AXP2101_BLDO_VOL0 0x96
#define AXP2101_BLDO_VOL1 0x97
#define AXP2101_CPUSLDO_VOL 0x98
#define AXP2101_DLDO_VOL0 0x99
#define AXP2101_DLDO_VOL1 0x9A

// Power enable control
#define AXP2101_LDO_ONOFF_CTRL0 0x90
#define AXP2101_LDO_ONOFF_CTRL1 0x91
#define AXP2101_DC_ONOFF_DVM_CTRL 0x81

// Battery/ADC
#define AXP2101_ADC_ENABLE 0x30
#define AXP2101_VBAT_H 0x34
#define AXP2101_VBAT_L 0x35
#define AXP2101_TS_H 0x36       // External TS pin (thermistor)
#define AXP2101_TS_L 0x37
#define AXP2101_VBUS_H 0x38
#define AXP2101_VBUS_L 0x39
#define AXP2101_INTERNAL_TEMP_H 0x3C  // Internal die temperature
#define AXP2101_INTERNAL_TEMP_L 0x3D
#define AXP2101_VSYS_H 0x3A
#define AXP2101_VSYS_L 0x3B

// Charge control (SWcharge-Doku)
#define AXP2101_CHG_IPRE   0x61  // Pre-Charge Current IPRECHG[3:0]
#define AXP2101_CHG_ICHG   0x62  // Fast-Charge Current ICHG[4:0]
#define AXP2101_CHG_ITERM  0x63  // Termination Current ITERM[3:0], Bit4 = disable
#define AXP2101_CHG_VOL    0x64  // Charge voltage profile VREG[2:0]

// IRQ control
#define AXP2101_IRQ_EN0 0x40
#define AXP2101_IRQ_EN1 0x41
#define AXP2101_IRQ_EN2 0x42
#define AXP2101_IRQ_STATUS0 0x48
#define AXP2101_IRQ_STATUS1 0x49
#define AXP2101_IRQ_STATUS2 0x4A

// IRQ bit masks (IRQ_STATUS0 / IRQ_EN0)
#define AXP2101_WARNING_LEVEL1_IRQ  (1 << 6)  // Battery level drops to warning level 1
#define AXP2101_WARNING_LEVEL2_IRQ  (1 << 7)  // Battery level drops to warning level 2

// IRQ bit masks (IRQ_STATUS1 / IRQ_EN1)
#define AXP2101_PKEY_POSITIVE_IRQ   (1 << 0)  // Power button pressed (falling edge)
#define AXP2101_PKEY_NEGATIVE_IRQ   (1 << 1)  // Power button released (rising edge)
#define AXP2101_PKEY_LONG_IRQ       (1 << 2)  // Power button long press
#define AXP2101_PKEY_SHORT_IRQ      (1 << 3)  // Power button short press

// Charging LED control
#define AXP2101_CHGLED_SET_CTRL 0x69

// Battery gauge
#define AXP2101_BAT_PERCENT 0xA4
#define AXP2101_CHG_FG_WDT_CTRL 0x18
// Power / charger / fuel gauge misc
#define AXP2101_COMMON_CONFIG      0x10   // Software power-off (bit0), restart (bit1)
#define AXP2101_BATFET_CTRL        0x12   // BATFET control
#define AXP2101_RESET_FUEL_GAUGE   0x17   // Reset the fuel gauge
#define AXP2101_CHG_FG_WDT_CTRL    0x18   // Charger, fuel gauge, watchdog on/off
#define AXP2101_BATT_DETECT_CTRL   0x68   // Battery detection control
// Misc
#define AXP2101_VBUS_VOL_SET 0x16
#define AXP2101_VBUS_CUR_SET 0x15
#define AXP2101_VSYS_MIN 0x17
// AXP2101_POK_SET was previously 0x23, which is actually DCDC OVP/UVP control.
// Power-key timing lives in IRQ_OFF_ON_LEVEL_CTRL (0x27): [5:4] IRQ level time,
// [3:2] power-off long press, [1:0] power-on press.
#define AXP2101_DC_OVP_UVP_CTRL 0x23
#define AXP2101_IRQ_OFF_ON_LEVEL_CTRL 0x27
#define AXP2101_SLEEP_CFG 0x26
#define AXP2101_WAKEUP_CFG 0x27
#define AXP2101_WDT_CTRL 0x29

enum AXP2101Model {
  AXP2101_WS_S3AMOLED175,
};

#define SLEEP_MSEC(us) (((uint64_t)us) * 1000L)
#define SLEEP_SEC(us)  (((uint64_t)us) * 1000000L)
#define SLEEP_MIN(us)  (((uint64_t)us) * 60L * 1000000L)
#define SLEEP_HR(us)   (((uint64_t)us) * 60L * 60L * 1000000L)

#define CURRENT_100MA  (0b0000)
#define CURRENT_190MA  (0b0001)
#define CURRENT_280MA  (0b0010)
#define CURRENT_360MA  (0b0011)
#define CURRENT_450MA  (0b0100)
#define CURRENT_550MA  (0b0101)
#define CURRENT_630MA  (0b0110)
#define CURRENT_700MA  (0b0111)

class AXP2101Component : public PollingComponent, public i2c::I2CDevice {
public:
  void set_batteryvoltage_sensor(sensor::Sensor *batteryvoltage_sensor) { batteryvoltage_sensor_ = batteryvoltage_sensor; }
  void set_batterylevel_sensor(sensor::Sensor *batterylevel_sensor) { batterylevel_sensor_ = batterylevel_sensor; }
  void set_vbusvoltage_sensor(sensor::Sensor *vbusvoltage_sensor) { vbusvoltage_sensor_ = vbusvoltage_sensor; }
  void set_temperature_sensor(sensor::Sensor *temperature_sensor) { temperature_sensor_ = temperature_sensor; }

  void set_batterycharging_bsensor(binary_sensor::BinarySensor *batterycharging_bsensor) { batterycharging_bsensor_ = batterycharging_bsensor; }
  void set_vbusconnected_bsensor(binary_sensor::BinarySensor *vbusconnected_bsensor) { vbusconnected_bsensor_ = vbusconnected_bsensor; }
  void set_pkey_short_bsensor(binary_sensor::BinarySensor *pkey_short_bsensor) { pkey_short_bsensor_ = pkey_short_bsensor; }
  void set_pkey_long_bsensor(binary_sensor::BinarySensor *pkey_long_bsensor) { pkey_long_bsensor_ = pkey_long_bsensor; }
  void set_pkey_positive_bsensor(binary_sensor::BinarySensor *pkey_positive_bsensor) { pkey_positive_bsensor_ = pkey_positive_bsensor; }
  void set_pkey_negative_bsensor(binary_sensor::BinarySensor *pkey_negative_bsensor) { pkey_negative_bsensor_ = pkey_negative_bsensor; }
  void set_low_battery_level1_bsensor(binary_sensor::BinarySensor *low_battery_level1_bsensor) { low_battery_level1_bsensor_ = low_battery_level1_bsensor; }
  void set_low_battery_level2_bsensor(binary_sensor::BinarySensor *low_battery_level2_bsensor) { low_battery_level2_bsensor_ = low_battery_level2_bsensor; }
  void set_model(AXP2101Model model) { this->model_ = model; }
  void set_charging_led_number(number::Number *charging_led_number) { charging_led_number_ = charging_led_number; }

  // Charging LED control
  void setChargingLedMode(uint8_t mode);  // 0=Off, 1=Blink 1Hz, 2=Blink 4Hz, 3=On, 4=Auto
  uint8_t getChargingLedMode();

  // Battery charging control (AXP2101 reg 0x18 bit1 = CHG_EN)
  // Used by the HA "keep battery in range" manager automation.
  void set_charging_switch(switch_::Switch *sw) { this->charging_switch_ = sw; }
  bool set_charging_enabled(bool enabled);
  bool is_charging_enabled();
  // Set the fast-charge constant current limit (AXP2101 reg 0x62, 5-bit code).
  // 0 = charging effectively disabled; see XpowersLib XPOWERS_AXP2101_CHG_CUR_*.
  bool set_charge_current(uint8_t code);
  uint8_t get_charge_current();

  // Battery safety floor: charging may not be disabled at or below this level,
  // and is re-enabled automatically if the pack falls to it. Prevents leaving
  // the device at 0% with charging off. Configurable via `min_charging_level`
  // (default 10%).
  void set_min_charging_level(float level) { this->min_charging_level_ = level; }
  float get_min_charging_level() const { return this->min_charging_level_; }
  // Current battery percentage (0-100), or NAN when no battery / unknown.
  float get_battery_level();
  // True when charging is allowed to be disabled (battery above the floor, or
  // level unknown).
  bool charging_disable_allowed();

  // ========== INTERNAL METHODS ==========
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override;
  void update() override;  // Called at update_interval for sensor readings
  void loop() override;    // Called every loop for fast button polling
  // User-callable helper: reset fuel gauge and clear NVS flag
  void reset_fuel_gauge_manual();
  void soft_power_off_esp();
  // True PMU soft power-off: REG10H[0]=1 turns off all rails (<20 uA).
  // Wake with the PWRON key (or VBUS insertion, if configured). Falls back to
  // ESP deep sleep if the PMU write fails.
  void power_off();

private:
  void check_interrupts();  // Fast interrupt/button check
    static std::string GetStartupReason();
    void configureChargerDefaults();

protected:
    sensor::Sensor *batteryvoltage_sensor_{nullptr};
    sensor::Sensor *batterylevel_sensor_{nullptr};
    sensor::Sensor *vbusvoltage_sensor_{nullptr};
    sensor::Sensor *temperature_sensor_{nullptr};
    binary_sensor::BinarySensor *batterycharging_bsensor_{nullptr};
    binary_sensor::BinarySensor *vbusconnected_bsensor_{nullptr};
    binary_sensor::BinarySensor *pkey_short_bsensor_{nullptr};
    binary_sensor::BinarySensor *pkey_long_bsensor_{nullptr};
    binary_sensor::BinarySensor *pkey_positive_bsensor_{nullptr};
    binary_sensor::BinarySensor *pkey_negative_bsensor_{nullptr};
    binary_sensor::BinarySensor *low_battery_level1_bsensor_{nullptr};
    binary_sensor::BinarySensor *low_battery_level2_bsensor_{nullptr};
    number::Number *charging_led_number_{nullptr};
    switch_::Switch *charging_switch_{nullptr};
    float min_charging_level_{10.0f};

    AXP2101Model model_;
    gpio_num_t irq_pin_{GPIO_NUM_21};
    ESPPreferenceObject fuel_gauge_pref_;

    /* 
     * M5Stack Core2 Values
     * DC1: Internal 3.3V
     * DC2: Internal 1.0V
     * DC3: Display power (3.3V)
     * DC4: Internal 1.0V
     * DC5: External 3.3V
     * ALDO1-4: Various LDOs
     * BLDO1: Backlight control
     * BLDO2: Secondary power
     */

    // Helper methods for register access
    bool writeRegister(uint8_t reg, uint8_t value);
    bool readRegister(uint8_t reg, uint8_t *value);
    bool readRegisterMulti(uint8_t reg, uint8_t *data, size_t len);
    bool setBits(uint8_t reg, uint8_t mask);
    bool clearBits(uint8_t reg, uint8_t mask);
    bool fuel_gauge_initialized_{false};
    // Number of post-boot updates for which the charger config is logged.
    // Component setup runs before WiFi/API is up, so a few update() cycles are
    // needed for the power-management state to be visible in the API logs.
    uint8_t charger_startup_logs_{0};

    // AXP2101 control methods
    void initPowerOutputs();
    void initCharger();
    void initInterrupts();
    uint16_t getBatteryVoltage();  // Returns mV
    uint8_t getBatteryPercent();
    bool isCharging();
    bool isBatteryConnected();
    bool isVBUSGood();
    uint16_t getVBUSVoltage();  // Returns mV
    float getTemperature();  // Returns °C
    uint8_t getChipID();
	
    // DC-DC converter control
    void setDCVoltage(uint8_t channel, uint16_t voltage_mv);
    void enableDC(uint8_t channel, bool enable);
    
    // LDO control
    void setALDOVoltage(uint8_t channel, uint16_t voltage_mv);
    void setBLDOVoltage(uint8_t channel, uint16_t voltage_mv);
    void setDLDOVoltage(uint8_t channel, uint16_t voltage_mv);
    void enableALDO(uint8_t channel, bool enable);
    void enableBLDO(uint8_t channel, bool enable);
    void enableDLDO(uint8_t channel, bool enable);

    //AXP2101Model getModel() const { return model_; }
    void SetSleep();
    void DeepSleep(uint64_t time_in_us = 0);
    void LightSleep(uint64_t time_in_us = 0);
};

}
}

#endif
