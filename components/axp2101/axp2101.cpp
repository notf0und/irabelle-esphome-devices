#include "axp2101.h"
#include "esphome/core/log.h"
#include "esphome/components/number/number.h"
#include "esphome/components/switch/switch.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include <cmath>

namespace esphome {
namespace axp2101 {

static const char *TAG = "axp2101.sensor";
static bool pmu_flag = false;

void IRAM_ATTR axp2101_isr_handler(void* arg) {
    pmu_flag = true;
}

// Helper methods for register access
bool AXP2101Component::writeRegister(uint8_t reg, uint8_t value) {
    return this->write_byte(reg, value);
}

bool AXP2101Component::readRegister(uint8_t reg, uint8_t *value) {
    return this->read_byte(reg, value);
}

bool AXP2101Component::readRegisterMulti(uint8_t reg, uint8_t *data, size_t len) {
    return this->read_bytes(reg, data, len);
}

bool AXP2101Component::setBits(uint8_t reg, uint8_t mask) {
    uint8_t value;
    if (!readRegister(reg, &value)) return false;
    value |= mask;
    return writeRegister(reg, value);
}

bool AXP2101Component::clearBits(uint8_t reg, uint8_t mask) {
    uint8_t value;
    if (!readRegister(reg, &value)) return false;
    value &= ~mask;
    return writeRegister(reg, value);
}

uint8_t AXP2101Component::getChipID() {
    uint8_t chip_id = 0;
    readRegister(AXP2101_CHIP_ID, &chip_id);
    return chip_id;
}

// DC-DC control methods
void AXP2101Component::setDCVoltage(uint8_t channel, uint16_t voltage_mv) {
    uint8_t reg = AXP2101_DC_VOL0 + channel;
    uint8_t value = 0;
    
    // DC1: 1500-3400mV, 100mV/step
    // DC2/3/4: Different ranges - simplified implementation
    if (channel == 0) { // DC1
        if (voltage_mv >= 1500 && voltage_mv <= 3400) {
            value = (voltage_mv - 1500) / 100;
        }
    } else if (channel <= 3) { // DC2-4
        if (voltage_mv >= 500 && voltage_mv <= 1200) {
            value = (voltage_mv - 500) / 10;
        } else if (voltage_mv >= 1220 && voltage_mv <= 1540) {
            value = 71 + (voltage_mv - 1220) / 20;
        }
    } else if (channel == 4) { // DC5
        if (voltage_mv >= 1400 && voltage_mv <= 3700) {
            value = (voltage_mv - 1400) / 100;
        } else if (voltage_mv == 1200) {
            value = 0;
        }
    }
    
    writeRegister(reg, value);
}

void AXP2101Component::enableDC(uint8_t channel, bool enable) {
    uint8_t mask = 1 << channel;
    if (enable) {
        setBits(AXP2101_DC_ONOFF_DVM_CTRL, mask);
    } else {
        clearBits(AXP2101_DC_ONOFF_DVM_CTRL, mask);
    }
}

// LDO control methods
void AXP2101Component::setALDOVoltage(uint8_t channel, uint16_t voltage_mv) {
    if (channel > 3) return;
    uint8_t reg = AXP2101_ALDO_VOL0 + channel;
    
    // ALDO: 500-3500mV, 100mV/step
    if (voltage_mv >= 500 && voltage_mv <= 3500) {
        uint8_t value = (voltage_mv - 500) / 100;
        writeRegister(reg, value);
    }
}

void AXP2101Component::setBLDOVoltage(uint8_t channel, uint16_t voltage_mv) {
    if (channel > 1) return;
    uint8_t reg = AXP2101_BLDO_VOL0 + channel;
    
    // BLDO: 500-3500mV, 100mV/step
    if (voltage_mv >= 500 && voltage_mv <= 3500) {
        uint8_t value = (voltage_mv - 500) / 100;
        writeRegister(reg, value);
    }
}

void AXP2101Component::enableALDO(uint8_t channel, bool enable) {
    if (channel > 3) return;
    uint8_t mask = 1 << channel;
    if (enable) {
        setBits(AXP2101_LDO_ONOFF_CTRL0, mask);
    } else {
        clearBits(AXP2101_LDO_ONOFF_CTRL0, mask);
    }
}

void AXP2101Component::enableBLDO(uint8_t channel, bool enable) {
    if (channel > 1) return;
    uint8_t mask = 1 << (channel + 4);  // BLDO1/2 are bits 4-5
    if (enable) {
        setBits(AXP2101_LDO_ONOFF_CTRL0, mask);
    } else {
        clearBits(AXP2101_LDO_ONOFF_CTRL0, mask);
    }
}

void AXP2101Component::setDLDOVoltage(uint8_t channel, uint16_t voltage_mv) {
    if (channel > 1) return;
    uint8_t reg = AXP2101_DLDO_VOL0 + channel;
    
    // DLDO: 500-3400mV, 100mV/step
    if (voltage_mv >= 500 && voltage_mv <= 3400) {
        uint8_t value = (voltage_mv - 500) / 100;
        writeRegister(reg, value);
    }
}

void AXP2101Component::enableDLDO(uint8_t channel, bool enable) {
    if (channel > 1) return;
    uint8_t mask = 1 << (channel + 7);  // DLDO1/2 are bits 7 and bit 0 of next register
    if (channel == 0) {
        // DLDO1 is bit 7 of LDO_ONOFF_CTRL0
        if (enable) {
            setBits(AXP2101_LDO_ONOFF_CTRL0, mask);
        } else {
            clearBits(AXP2101_LDO_ONOFF_CTRL0, mask);
        }
    } else {
        // DLDO2 is bit 0 of LDO_ONOFF_CTRL1
        if (enable) {
            setBits(AXP2101_LDO_ONOFF_CTRL1, 0x01);
        } else {
            clearBits(AXP2101_LDO_ONOFF_CTRL1, 0x01);
        }
    }
}

// Battery and charging methods
uint16_t AXP2101Component::getBatteryVoltage() {
    uint8_t hi = 0, lo = 0;
    if (!readRegister(AXP2101_VBAT_H, &hi) || !readRegister(AXP2101_VBAT_L, &lo)) {
        ESP_LOGW(TAG, "Failed to read VBAT registers");
        return 0;
    }

    // 14-bit, 1 mV/LSB
    uint16_t raw14 = ((hi & 0x3F) << 8) | lo;
    ESP_LOGD(TAG, "VBAT raw14=%u (H=0x%02X L=0x%02X)", raw14, hi, lo);

    return raw14;  // in mV
}

uint8_t AXP2101Component::getBatteryPercent() {
    uint8_t percent = 0;
    if (!readRegister(AXP2101_BAT_PERCENT, &percent)) {
        ESP_LOGW(TAG, "Failed to read battery percentage (0xA4)");
        return 0;
    }
    ESP_LOGD(TAG, "Raw fuel-gauge percentage reg 0xA4 = 0x%02X (%u)", percent, percent);
    return percent;  // 0..100 laut Datenblatt
}

bool AXP2101Component::isCharging() {
    uint8_t stat0 = 0;
    if (!readRegister(AXP2101_COMM_STAT0, &stat0)) {
        ESP_LOGW(TAG, "Failed to read COMM_STAT0 for charging state");
        return false;
    }

    uint8_t cs = stat0 & 0x07;
    bool chg = (cs <= 0b011);  // 0..3 = charging states
    ESP_LOGD(TAG, "Charging state bits=%u (COMM_STAT0=0x%02X) -> charging=%d", cs, stat0, chg);
    return chg;
}

bool AXP2101Component::isBatteryConnected() {
    uint8_t status0 = 0;
    if (!readRegister(AXP2101_COMM_CFG, &status0)) {  // 0x00
        ESP_LOGW(TAG, "Failed to read COMM_CFG for battery present state");
        return false;
    }
    bool present = (status0 & 0x08) != 0;  // Bit 3
    ESP_LOGD(TAG, "Battery present state: %d (COMM_CFG=0x%02X)", present, status0);
    return present;
}

bool AXP2101Component::isVBUSGood() {
    uint8_t cfg = 0;
    if (!readRegister(AXP2101_COMM_CFG, &cfg)) {
        ESP_LOGW(TAG, "Failed to read COMM_CFG for VBUS");
        return false;
    }

    bool vbus_good = (cfg & (1 << 5));
    ESP_LOGD(TAG, "VBUS good=%d (COMM_CFG=0x%02X)", vbus_good, cfg);
    return vbus_good;
}

void AXP2101Component::reset_fuel_gauge_manual() {
    ESP_LOGW(TAG, "Manual fuel gauge reset requested");
    writeRegister(AXP2101_RESET_FUEL_GAUGE, 0b00001000);

    // Flag neu setzen, damit wir beim nächsten Boot NICHT noch mal automatisch resetten
    uint8_t fg_flag = 0xA5;
    if (this->fuel_gauge_pref_.save(&fg_flag)) {
        ESP_LOGD(TAG, "Fuel gauge init flag updated after manual reset");
    }
}

void AXP2101Component::setChargingLedMode(uint8_t mode) {
    if (mode > 4) return;  // Valid modes: 0-4
    
    uint8_t val = 0;
    if (!readRegister(AXP2101_CHGLED_SET_CTRL, &val)) {
        ESP_LOGW(TAG, "Failed to read charging LED control register");
        return;
    }
    
    // Clear bits 4-5 (mode) and bit 2 (manual control enable)
    val &= 0xC8;  // Keep bits 7,6,3,1,0 unchanged
    
    if (mode == 4) {
        // Mode 4: Automatic control by charger (bit 2 = 0)
        val |= 0x01;  // Set bit 0 to enable LED function
    } else {
        // Modes 0-3: Manual control (bit 2 = 1)
        val |= 0x05;  // Set bits 2,0 for manual mode and LED enable
        val |= (mode << 4);  // Set mode in bits 4-5
    }
    
    writeRegister(AXP2101_CHGLED_SET_CTRL, val);
    ESP_LOGD(TAG, "Set charging LED mode to %d (reg value: 0x%02X)", mode, val);
}

uint8_t AXP2101Component::getChargingLedMode() {
    uint8_t val = 0;
    if (!readRegister(AXP2101_CHGLED_SET_CTRL, &val)) {
        return 0;
    }
    
    // Check if manual mode (bit 2 = 1) or automatic (bit 2 = 0)
    if (val & 0x04) {
        // Manual mode: return mode from bits 4-5
        return (val >> 4) & 0x03;
    } else {
        // Automatic mode
        return 4;
    }
}

uint16_t AXP2101Component::getVBUSVoltage() {
    uint8_t data[2];
    if (!readRegisterMulti(AXP2101_VBUS_H, data, 2)) {
        ESP_LOGW(TAG, "Failed to read VBUS voltage registers");
        return 0;
    }
    // Per XPowersLib: readRegisterH6L8 - 14-bit value (6 high bits + 8 low bits)
    // Result is in millivolts (no conversion factor needed)
    uint16_t raw = ((uint16_t)(data[0] & 0x3F) << 8) | data[1];
    ESP_LOGD(TAG, "VBUS raw value: %d mV (H:0x%02X L:0x%02X)", raw, data[0], data[1]);
    return raw;
}

float AXP2101Component::getTemperature() {
    uint8_t data[2];
    
    // Internal die temperature registers: 0x3C (high byte), 0x3D (low byte)
    // Formula: T(°C) = 22.0 + (7274 - raw) / 20.0
    // Note: 0x36/0x37 are for external TS pin (thermistor), not internal temp
    if (!readRegisterMulti(AXP2101_INTERNAL_TEMP_H, data, 2)) {
        return 0.0f;
    }
    
    // 16-bit value: big-endian format (high byte first)
    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    float temp = 22.0f + (7274.0f - (float)raw) / 20.0f;
    return temp;
}

void AXP2101Component::initPowerOutputs() {
    ESP_LOGI(TAG, "Initializing power outputs...");

    uint8_t dc_state, ldo_state0, ldo_state1;
    readRegister(AXP2101_DC_ONOFF_DVM_CTRL, &dc_state);
    readRegister(AXP2101_LDO_ONOFF_CTRL0, &ldo_state0);
    readRegister(AXP2101_LDO_ONOFF_CTRL1, &ldo_state1);

    ESP_LOGD(TAG, "Current power states - DC: 0x%02X, LDO0: 0x%02X, LDO1: 0x%02X",
             dc_state, ldo_state0, ldo_state1);

    // Legacy from the 1.75" LCD board where DLDO1 drove the backlight.
    // The 2.16" AMOLED has no backlight GPIO (official BSP:
    // BSP_LCD_BACKLIGHT = GPIO_NUM_NC) and brightness is the CO5300 panel
    // register 0x51, driven from the display component. DLDO1 is left at a
    // fixed 3.4 V enable here only to preserve the previous boot state; it is
    // no longer tied to brightness. See docs/WAVESHARE_AMOLED_216_PLAN.md.
    setDLDOVoltage(0, 3400);
    bool dldo1_enabled = (ldo_state0 & (1 << 7));
    ESP_LOGD(TAG, "DLDO1 status before: %s", dldo1_enabled ? "ENABLED" : "DISABLED");
    if (!dldo1_enabled) {
        ESP_LOGD(TAG, "Enabling DLDO1 (fixed)...");
        enableDLDO(0, true);
    }

    ESP_LOGI(TAG, "Power outputs initialized.");
}

void AXP2101Component::initCharger() {
    ESP_LOGI(TAG, "Initializing charger...");

    configureChargerDefaults();

    // --- Fuel-Gauge Modul explizit aktivieren (REG 0x18, Bit 3) ---
    uint8_t fg_ctrl = 0;
    if (readRegister(AXP2101_CHG_FG_WDT_CTRL, &fg_ctrl)) {
        if (!(fg_ctrl & 0x08)) {
            ESP_LOGI(TAG, "Enabling fuel gauge module (REG 0x18 was 0x%02X)", fg_ctrl);
            writeRegister(AXP2101_CHG_FG_WDT_CTRL, fg_ctrl | 0x08);
        } else {
            ESP_LOGD(TAG, "Fuel gauge module already enabled (REG 0x18 = 0x%02X)", fg_ctrl);
        }
    } else {
        ESP_LOGW(TAG, "Failed to read REG 0x18 for fuel gauge control");
    }

    ESP_LOGI(TAG, "Charger initialized (basic ADC + fuel gauge enabled)");
}


void AXP2101Component::configureChargerDefaults() {
    // The 2.16" board has no battery NTC on the TS pin. If TS measurement is
    // left enabled the AXP2101 samples a floating pin, sees an out-of-range
    // "temperature" and throttles/suspends charging. Waveshare's own firmware
    // disables it for exactly this reason
    // (examples/esp-idf/01_AXP2101, port_axp2101.cpp).
    clearBits(AXP2101_ADC_ENABLE, 0x02);  // 0x30 bit1 = TS pin measure enable -> off

    // ADC channels used by the exposed sensors (reg 0x30):
    //   bit4 = die temperature, bit3 = VSYS, bit2 = VBUS, bit0 = battery voltage
    setBits(AXP2101_ADC_ENABLE, 0x1D);

    // Charge parameters, matching the official Waveshare firmware:
    //   precharge 50 mA, fast charge 400 mA, termination 25 mA, target 4.2 V
    writeRegister(AXP2101_CHG_IPRE, 0x02);   // 0x61: 50 mA  (25 mA * N)
    writeRegister(AXP2101_CHG_ICHG, 0x0A);   // 0x62: 400 mA
    writeRegister(AXP2101_CHG_ITERM, 0x11);  // 0x63: iterm enable (bit4) + 25 mA
    writeRegister(AXP2101_CHG_VOL, 0x03);    // 0x64: 4.2 V target

    // Read back the applied configuration so it is verifiable from the logs.
    uint8_t adc = 0, ipre = 0, ichg = 0, iterm = 0, cv = 0;
    readRegister(AXP2101_ADC_ENABLE, &adc);
    readRegister(AXP2101_CHG_IPRE, &ipre);
    readRegister(AXP2101_CHG_ICHG, &ichg);
    readRegister(AXP2101_CHG_ITERM, &iterm);
    readRegister(AXP2101_CHG_VOL, &cv);
    ESP_LOGI(TAG, "Charger config readback: 0x30=0x%02X (TS ch=%u) 0x61=0x%02X 0x62=0x%02X 0x63=0x%02X 0x64=0x%02X",
             adc, (adc >> 1) & 0x01, ipre, ichg, iterm, cv);
}

bool AXP2101Component::set_charging_enabled(bool enabled) {
    uint8_t reg = 0;
    if (!readRegister(AXP2101_CHG_FG_WDT_CTRL, &reg)) {
        ESP_LOGW(TAG, "Failed to read CHG_FG_WDT_CTRL (0x18) for charging control");
        return false;
    }
    if (enabled) {
        reg |= 0x02;  // bit1 = CHG_EN, cell battery charge enable
    } else {
        reg &= ~0x02;
    }
    ESP_LOGD(TAG, "Battery charging %s (0x18=0x%02X)", enabled ? "enabled" : "disabled", reg);
    return writeRegister(AXP2101_CHG_FG_WDT_CTRL, reg);
}

bool AXP2101Component::is_charging_enabled() {
    uint8_t reg = 0;
    if (!readRegister(AXP2101_CHG_FG_WDT_CTRL, &reg)) {
        return false;
    }
    return (reg & 0x02) != 0;
}

bool AXP2101Component::set_charge_current(uint8_t code) {
    if (code > 0x10) return false;  // 0x10 = 1000 mA is the highest valid code
    uint8_t reg = 0;
    if (!readRegister(AXP2101_CHG_ICHG, &reg)) {
        return false;
    }
    reg = (reg & 0xE0) | (code & 0x1F);
    return writeRegister(AXP2101_CHG_ICHG, reg);
}

uint8_t AXP2101Component::get_charge_current() {
    uint8_t reg = 0;
    if (!readRegister(AXP2101_CHG_ICHG, &reg)) {
        return 0;
    }
    return reg & 0x1F;
}

float AXP2101Component::get_battery_level() {
    if (!isBatteryConnected()) {
        return NAN;
    }
    float level = (float) getBatteryPercent();
    if (level == 0.0f) {
        // Fuel gauge returned 0 - fall back to a voltage estimate (same as update()).
        uint16_t vbat_mv = getBatteryVoltage();
        float vbat_v = vbat_mv / 1000.0f;
        level = (vbat_v - 3.0f) / (4.1f - 3.0f) * 100.0f;
        if (level > 100.0f) level = 100.0f;
        if (level < 0.0f) level = 0.0f;
    }
    return level;
}

bool AXP2101Component::charging_disable_allowed() {
    float level = get_battery_level();
    // Unknown level (e.g. no battery): nothing to protect, allow the change.
    if (std::isnan(level)) {
        return true;
    }
    return level > min_charging_level_;
}

void AXP2101Component::initInterrupts() {
    ESP_LOGI(TAG, "Initializing interrupts...");
    
    // Power-key timing lives in IRQ_OFF_ON_LEVEL_CTRL (0x27):
    //   [5:4] IRQ level time, [3:2] power-off long press, [1:0] power-on press
    //   on: 0=128ms 1=512ms 2=1s 3=2s   off: 0=4s 1=6s 2=8s 3=10s
    // The previous code wrote POK values to 0x23 (which is DCDC OVP/UVP
    // control) and wrote 0x03 to 0x27, forcing the longest 2 s power-on hold -
    // that is why waking the device took so long.
    uint8_t pok = 0;
    if (readRegister(AXP2101_IRQ_OFF_ON_LEVEL_CTRL, &pok)) {
        pok &= ~0x0F;        // keep IRQ level time bits [5:4]
        pok |= (0x01 << 2);  // power-off long press: 6 s
        pok |= 0x01;         // power-on press: 512 ms
        writeRegister(AXP2101_IRQ_OFF_ON_LEVEL_CTRL, pok);
        ESP_LOGI(TAG, "POK timing set: power-on 512ms, power-off 6s (0x27=0x%02X)", pok);
    } else {
        ESP_LOGW(TAG, "Failed to read IRQ_OFF_ON_LEVEL_CTRL (0x27)");
    }
    // Skip GPIO interrupt setup for now - can be added later if needed
    ESP_LOGD(TAG, "Skipping GPIO IRQ pin setup (not critical for basic operation)");
    
    // Clear all interrupt flags
    writeRegister(AXP2101_IRQ_STATUS0, 0xFF);
    writeRegister(AXP2101_IRQ_STATUS1, 0xFF);
    writeRegister(AXP2101_IRQ_STATUS2, 0xFF);
    
    // Disable all interrupts first
    writeRegister(AXP2101_IRQ_EN0, 0x00);
    writeRegister(AXP2101_IRQ_EN1, 0x00);
    writeRegister(AXP2101_IRQ_EN2, 0x00);
    
    // Enable low battery warning interrupts if any binary sensors are configured
    uint8_t irq_en0 = 0x00;
    if (low_battery_level1_bsensor_ != nullptr) {
        irq_en0 |= AXP2101_WARNING_LEVEL1_IRQ;
        ESP_LOGD(TAG, "Enabled low battery warning level 1 interrupt");
    }
    if (low_battery_level2_bsensor_ != nullptr) {
        irq_en0 |= AXP2101_WARNING_LEVEL2_IRQ;
        ESP_LOGD(TAG, "Enabled low battery warning level 2 interrupt");
    }
    
    if (irq_en0 != 0x00) {
        writeRegister(AXP2101_IRQ_EN0, irq_en0);
        ESP_LOGI(TAG, "Low battery warning interrupts enabled: 0x%02X", irq_en0);
    }
    
    // Enable power button interrupts if any binary sensors are configured
    uint8_t irq_en1 = 0x00;
    if (pkey_short_bsensor_ != nullptr) {
        irq_en1 |= AXP2101_PKEY_SHORT_IRQ;
        ESP_LOGD(TAG, "Enabled PKEY short press interrupt");
    }
    if (pkey_long_bsensor_ != nullptr) {
        irq_en1 |= AXP2101_PKEY_LONG_IRQ;
        ESP_LOGD(TAG, "Enabled PKEY long press interrupt");
    }
    if (pkey_positive_bsensor_ != nullptr) {
        // Enable both positive and negative edge to track button hold state
        irq_en1 |= AXP2101_PKEY_POSITIVE_IRQ | AXP2101_PKEY_NEGATIVE_IRQ;
        ESP_LOGD(TAG, "Enabled PKEY positive/negative edge interrupts");
    }
    if (pkey_negative_bsensor_ != nullptr) {
        // Already enabled above if pkey_positive exists, but enable if standalone
        irq_en1 |= AXP2101_PKEY_NEGATIVE_IRQ;
        ESP_LOGD(TAG, "Enabled PKEY negative edge interrupt");
    }
    
    if (irq_en1 != 0x00) {
        writeRegister(AXP2101_IRQ_EN1, irq_en1);
        ESP_LOGI(TAG, "Power button interrupts enabled: 0x%02X", irq_en1);
        
        // Verify the register was written correctly
        uint8_t verify = 0;
        readRegister(AXP2101_IRQ_EN1, &verify);
        ESP_LOGI(TAG, "IRQ_EN1 verification readback: 0x%02X", verify);
    } else {
        ESP_LOGD(TAG, "No power button sensors configured, interrupts disabled");
    }
    
    ESP_LOGI(TAG, "Interrupts initialized (polling mode)");
}

void AXP2101Component::setup() {
    ESP_LOGCONFIG(TAG, "Setting up AXP2101...");
    ESP_LOGCONFIG(TAG, "I2C Address: 0x%02X", this->address_);
    
    // Small delay to allow I2C bus to stabilize
    delay_microseconds_safe(100000);  // 100ms
    
    // Test basic I2C communication by trying to read multiple registers
    ESP_LOGD(TAG, "Testing I2C communication...");
    uint8_t test_val;
    
    // Try reading chip ID first (register 0x03)
    bool comm_ok = readRegister(AXP2101_CHIP_ID, &test_val);
    if (comm_ok) {
        ESP_LOGD(TAG, "Successfully read Chip ID register: 0x%02X", test_val);
    } else {
        ESP_LOGW(TAG, "Failed to read Chip ID register");
    }
    
    // Try reading status register (register 0x01)
    if (readRegister(AXP2101_COMM_STAT0, &test_val)) {
        ESP_LOGD(TAG, "Successfully read Status register: 0x%02X", test_val);
        comm_ok = true;
    } else {
        ESP_LOGW(TAG, "Failed to read Status register");
    }
    
    // Try reading config register (register 0x00)
    if (readRegister(AXP2101_COMM_CFG, &test_val)) {
        ESP_LOGD(TAG, "Successfully read Config register: 0x%02X", test_val);
        comm_ok = true;
    } else {
        ESP_LOGW(TAG, "Failed to read Config register");
    }
    
    if (!comm_ok) {
        ESP_LOGE(TAG, "Failed to communicate with AXP2101!");
        ESP_LOGE(TAG, "Please check:");
        ESP_LOGE(TAG, "  - I2C bus is correctly initialized (SDA: GPIO21, SCL: GPIO22)");
        ESP_LOGE(TAG, "  - I2C address is correct (0x34 for AXP2101)");
        ESP_LOGE(TAG, "  - Power is supplied to the AXP2101");
        ESP_LOGE(TAG, "  - Pull-up resistors are present on I2C lines");
        this->mark_failed();
        return;
    }
    
    // Read and verify chip ID
    uint8_t chip_id = getChipID();
    ESP_LOGCONFIG(TAG, "Chip ID: 0x%02X", chip_id);
    
    // AXP2101 chip ID should be 0x47, but some variants may differ
    // 0x4A is a valid AXP2101 variant chip ID
    if (chip_id == 0x00 || chip_id == 0xFF) {
        ESP_LOGE(TAG, "Invalid chip ID: 0x%02X - possible I2C communication error", chip_id);
        this->mark_failed();
        return;
    } else if (chip_id == 0x47 || chip_id == 0x4A) {
        ESP_LOGI(TAG, "Valid AXP2101 chip ID detected: 0x%02X", chip_id);
    } else {
        ESP_LOGW(TAG, "Unexpected chip ID! Expected 0x47 or 0x4A, got 0x%02X - continuing anyway", chip_id);
    }
    // ---------------------------------------------------------------------
    // Enable fuel gauge, BATFET and battery detection
    // ---------------------------------------------------------------------
    ESP_LOGI(TAG, "Configuring fuel gauge, BATFET and battery detection...");

    // 0x18: Charger, fuel gauge, watchdog on/off control
    // Bit3 = Gauge enable, Bit1 = Cell battery charge enable (CHG_EN)
    uint8_t r18 = 0;
    if (readRegister(AXP2101_CHG_FG_WDT_CTRL, &r18)) {
        r18 |= 0b00001000;  // gauge ON
        // Bit1 (CHG_EN) is owned by the charging switch when one is configured,
        // so its restored state is not clobbered on every boot. Without a
        // switch, keep charging enabled.
        if (this->charging_switch_ == nullptr) {
            r18 |= 0b00000010;
        }
        writeRegister(AXP2101_CHG_FG_WDT_CTRL, r18);
        ESP_LOGD(TAG, "CHG_FG_WDT_CTRL (0x18) set to 0x%02X", r18);
    } else {
        ESP_LOGW(TAG, "Failed to read CHG_FG_WDT_CTRL (0x18)");
    }

    // 0x12: BATFET control – ensure battery is actually connected to system
    uint8_t r12 = 0;
    if (readRegister(AXP2101_BATFET_CTRL, &r12)) {
        r12 |= 0b00000010;  // Bit1 = BATFET ON
        writeRegister(AXP2101_BATFET_CTRL, r12);
        ESP_LOGD(TAG, "BATFET_CTRL (0x12) set to 0x%02X", r12);
    } else {
        ESP_LOGW(TAG, "Failed to read BATFET_CTRL (0x12)");
    }

    // 0x68: Battery detection control – make sure detection is enabled (bit0=0)
    uint8_t r68 = 0;
    if (readRegister(AXP2101_BATT_DETECT_CTRL, &r68)) {
        r68 &= ~0b00000001;  // bit0 = 0 → detection enabled
        writeRegister(AXP2101_BATT_DETECT_CTRL, r68);
        ESP_LOGD(TAG, "BATT_DETECT_CTRL (0x68) set to 0x%02X", r68);
    } else {
        ESP_LOGW(TAG, "Failed to read BATT_DETECT_CTRL (0x68)");
    }

    // ---------------------------------------------------------------------
    // One-time fuel gauge reset (0x17) – only on very first boot
    // ---------------------------------------------------------------------
    uint8_t fg_flag = 0;
    this->fuel_gauge_pref_ =
        global_preferences->make_preference<uint8_t>(0xA210100UL | this->address_);

    if (!this->fuel_gauge_pref_.load(&fg_flag) || fg_flag != 0xA5) {
        ESP_LOGI(TAG, "Fuel gauge not initialized yet, performing one-time reset (REG 0x17)...");
        writeRegister(AXP2101_RESET_FUEL_GAUGE, 0b00001000);  // Bit3 = reset gauge
        fg_flag = 0xA5;
        this->fuel_gauge_pref_.save(&fg_flag);
        this->fuel_gauge_initialized_ = true;
    } else {
        ESP_LOGI(TAG, "Fuel gauge already initialized, skipping reset");
        this->fuel_gauge_initialized_ = true;
    }
    
    // Initialize subsystems
    ESP_LOGI(TAG, "Initializing AXP2101 subsystems...");
    initPowerOutputs();
    initCharger();
    initInterrupts();
    
    // Read and publish current charging LED mode
    if (this->charging_led_number_ != nullptr) {
        uint8_t current_mode = getChargingLedMode();
        ESP_LOGD(TAG, "Current charging LED mode: %d", current_mode);
        this->charging_led_number_->publish_state(current_mode);
    }
    
    // Initialize state-based binary sensors to OFF
    // (Don't initialize momentary sensors like short/long press - they auto-clear)
    if (this->low_battery_level1_bsensor_ != nullptr) {
        this->low_battery_level1_bsensor_->publish_state(false);
    }
    if (this->low_battery_level2_bsensor_ != nullptr) {
        this->low_battery_level2_bsensor_->publish_state(false);
    }
    
    // Initialize button sensors with correct default states
    // pkey_positive is inverted: true=not pressed (default), false=pressed
    if (this->pkey_positive_bsensor_ != nullptr) {
        this->pkey_positive_bsensor_->publish_state(false);  // Default to OFF (not pressed)
    }
    if (this->pkey_negative_bsensor_ != nullptr) {
        this->pkey_negative_bsensor_->publish_state(false);  // Default to OFF
    }
    if (this->pkey_short_bsensor_ != nullptr) {
        this->pkey_short_bsensor_->publish_state(false);  // Default to OFF
    }
    if (this->pkey_long_bsensor_ != nullptr) {
        this->pkey_long_bsensor_->publish_state(false);  // Default to OFF
    }
    
    ESP_LOGCONFIG(TAG, "AXP2101 setup complete");
}

void AXP2101Component::dump_config() {
    ESP_LOGCONFIG(TAG, "AXP2101:");
    LOG_I2C_DEVICE(this);
    
    if (this->is_failed()) {
        ESP_LOGE(TAG, "Communication with AXP2101 failed!");
        return;
    }
    
    ESP_LOGCONFIG(TAG, "  Chip ID: 0x%02X", getChipID());
    LOG_SENSOR("  ", "Battery Voltage", this->batteryvoltage_sensor_);
    LOG_SENSOR("  ", "Battery Level", this->batterylevel_sensor_);
    LOG_BINARY_SENSOR("  ", "Battery Charging", this->batterycharging_bsensor_);
    LOG_SENSOR("  ", "VBUS Voltage", this->vbusvoltage_sensor_);
    LOG_BINARY_SENSOR("  ", "VBUS Connected", this->vbusconnected_bsensor_);
    LOG_SENSOR("  ", "Temperature", this->temperature_sensor_);
    ESP_LOGCONFIG(TAG, "  Charging safety floor: %.0f%%", this->min_charging_level_);
}

float AXP2101Component::get_setup_priority() const { 
    return setup_priority::DATA; 
}

void AXP2101Component::loop() {
    // Fast polling for interrupts/buttons - runs every loop cycle
    check_interrupts();
}

void AXP2101Component::check_interrupts() {
    // Read interrupt status registers to check for events
    uint8_t irq[3];
    readRegister(AXP2101_IRQ_STATUS0, &irq[0]);
    readRegister(AXP2101_IRQ_STATUS1, &irq[1]);
    readRegister(AXP2101_IRQ_STATUS2, &irq[2]);
    
    // Check for low battery warning events in IRQ_STATUS0
    if (irq[0] != 0x00) {
        ESP_LOGD(TAG, "IRQ_STATUS0: 0x%02X", irq[0]);
        
        // Warning level 1 (higher threshold, first warning)
        if ((irq[0] & AXP2101_WARNING_LEVEL1_IRQ) && low_battery_level1_bsensor_ != nullptr) {
            ESP_LOGW(TAG, "Low battery warning level 1!");
            low_battery_level1_bsensor_->publish_state(true);
        }
        
        // Warning level 2 (lower threshold, critical warning)
        if ((irq[0] & AXP2101_WARNING_LEVEL2_IRQ) && low_battery_level2_bsensor_ != nullptr) {
            ESP_LOGW(TAG, "Low battery warning level 2 (critical)!");
            low_battery_level2_bsensor_->publish_state(true);
        }
        
        // Clear the interrupt flags we just processed
        writeRegister(AXP2101_IRQ_STATUS0, irq[0]);
    }
    
    // Check for power button events in IRQ_STATUS1
    if (irq[1] != 0x00) {
        ESP_LOGD(TAG, "IRQ_STATUS1: 0x%02X", irq[1]);
        
        // Short press event
        if ((irq[1] & AXP2101_PKEY_SHORT_IRQ) && pkey_short_bsensor_ != nullptr) {
            ESP_LOGD(TAG, "Power button short press");
            pkey_short_bsensor_->publish_state(true);
            delay_microseconds_safe(50000);  // 50ms delay to ensure event is registered
            pkey_short_bsensor_->publish_state(false);
        }
        
        // Long press event
        if ((irq[1] & AXP2101_PKEY_LONG_IRQ) && pkey_long_bsensor_ != nullptr) {
            ESP_LOGD(TAG, "Power button long press");
            pkey_long_bsensor_->publish_state(true);
            delay_microseconds_safe(50000);  // 50ms delay to ensure event is registered
            pkey_long_bsensor_->publish_state(false);
        }
        
        // Positive edge (button pressed down)
        if ((irq[1] & AXP2101_PKEY_POSITIVE_IRQ) && pkey_positive_bsensor_ != nullptr) {
            ESP_LOGD(TAG, "Power button pressed (positive edge) - setting sensor OFF");
            pkey_positive_bsensor_->publish_state(false);  // OFF - IRQ semantics are inverted
        }
        
        // Negative edge (button released)
        if ((irq[1] & AXP2101_PKEY_NEGATIVE_IRQ)) {
            ESP_LOGD(TAG, "Power button released (negative edge) - setting sensor ON");
            // Set the positive edge sensor (button no longer held)
            if (pkey_positive_bsensor_ != nullptr) {
                pkey_positive_bsensor_->publish_state(true);  // ON - IRQ semantics are inverted
                ESP_LOGD(TAG, "pkey_positive sensor set to ON");
            }
            // Trigger the negative edge sensor momentarily
            if (pkey_negative_bsensor_ != nullptr) {
                pkey_negative_bsensor_->publish_state(true);
                delay_microseconds_safe(50000);  // 50ms delay
                pkey_negative_bsensor_->publish_state(false);
                ESP_LOGD(TAG, "pkey_negative sensor pulsed");
            }
        }
        
        // Clear the interrupt flags we just processed
        writeRegister(AXP2101_IRQ_STATUS1, irq[1]);
    }
    
    // Clear other interrupt flags if present
    if (irq[2] != 0x00) {
        writeRegister(AXP2101_IRQ_STATUS2, irq[2]);
    }
}

void AXP2101Component::update() {
    // Sensor readings - runs at update_interval (can be slow, e.g., 60s)

    // Log the live charger configuration for the first few update cycles after
    // boot. Component setup() runs before WiFi/API is up, so this is the only
    // way to see the power-management state in the API logs.
    if (this->charger_startup_logs_ < 6) {
        this->charger_startup_logs_++;
        uint8_t adc = 0, ichg = 0, iterm = 0, cv = 0, ctrl18 = 0, pok = 0;
        readRegister(AXP2101_ADC_ENABLE, &adc);
        readRegister(AXP2101_CHG_ICHG, &ichg);
        readRegister(AXP2101_CHG_ITERM, &iterm);
        readRegister(AXP2101_CHG_VOL, &cv);
        readRegister(AXP2101_CHG_FG_WDT_CTRL, &ctrl18);
        readRegister(AXP2101_IRQ_OFF_ON_LEVEL_CTRL, &pok);
        uint8_t code = ichg & 0x1F;
        uint16_t ma = code < 8 ? code * 25 : 200 + (code - 8) * 100;
        ESP_LOGI(TAG, "Power config: 0x30=0x%02X (TS_ch=%u) 0x62=0x%02X (%umA) 0x63=0x%02X 0x64=0x%02X CHG_EN=%u POK 0x27=0x%02X",
                 adc, (adc >> 1) & 0x01, ichg, ma, iterm, cv, (ctrl18 >> 1) & 0x01, pok);
    }

    // Safety floor: if charging was disabled and the pack has fallen to the
    // cutoff (or below), re-enable charging automatically. This runs on the
    // firmware side so the device cannot be stranded at 0% even if HA is down.
    if (this->charging_switch_ != nullptr && !this->is_charging_enabled()) {
        float level = get_battery_level();
        if (!std::isnan(level) && level <= min_charging_level_) {
            ESP_LOGW(TAG, "Battery at %.0f%% (<= %.0f%%): re-enabling charging (safety floor)",
                     level, min_charging_level_);
            this->charging_switch_->turn_on();
        }
    }

    // Update battery voltage
    if (this->batteryvoltage_sensor_ != nullptr) {
        uint16_t vbat_mv = getBatteryVoltage();
        float vbat_v = vbat_mv / 1000.0f;
        ESP_LOGD(TAG, "Battery Voltage: %.3fV (%dmV)", vbat_v, vbat_mv);
        this->batteryvoltage_sensor_->publish_state(vbat_v);
    }
    
    // Update battery level
    if (this->batterylevel_sensor_ != nullptr) {
        float battery_level = 0.0f;
        
        if (isBatteryConnected()) {
            battery_level = getBatteryPercent();
            
            // If battery percent register returns 0, fall back to voltage-based calculation
            if (battery_level == 0) {
                uint16_t vbat_mv = getBatteryVoltage();
                float vbat_v = vbat_mv / 1000.0f;
                battery_level = (vbat_v - 3.0f) / (4.1f - 3.0f) * 100.0f;
                if (battery_level > 100.0f) battery_level = 100.0f;
                if (battery_level < 0.0f) battery_level = 0.0f;
            }
            
            ESP_LOGD(TAG, "Battery Level: %.0f%%", battery_level);
            this->batterylevel_sensor_->publish_state(battery_level);
        } else {
            // No battery connected - report NaN to indicate unavailable
            ESP_LOGD(TAG, "Battery Level: No battery connected");
            this->batterylevel_sensor_->publish_state(NAN);
        }
        
        // Clear low battery warnings if battery level improves
        // Typically level 1 = ~15%, level 2 = ~5%
        if (battery_level > 15.0f && low_battery_level1_bsensor_ != nullptr) {
            low_battery_level1_bsensor_->publish_state(false);
        }
        if (battery_level > 5.0f && low_battery_level2_bsensor_ != nullptr) {
            low_battery_level2_bsensor_->publish_state(false);
        }
    }
    
    // Update charging status
    if (this->batterycharging_bsensor_ != nullptr) {
        bool charging = isCharging();
        ESP_LOGD(TAG, "Battery Charging: %s", charging ? "Yes" : "No");
        this->batterycharging_bsensor_->publish_state(charging);
    }
    
    // Update VBUS voltage
    if (this->vbusvoltage_sensor_ != nullptr) {
        uint16_t vbus_mv = getVBUSVoltage();
        float vbus_v = vbus_mv / 1000.0f;
        ESP_LOGD(TAG, "VBUS Voltage: %.3fV (%dmV)", vbus_v, vbus_mv);
        this->vbusvoltage_sensor_->publish_state(vbus_v);
    }
    
    // Update VBUS connected status
    if (this->vbusconnected_bsensor_ != nullptr) {
        bool vbus_good = isVBUSGood();
        this->vbusconnected_bsensor_->publish_state(vbus_good);
    }
    
    // Update temperature
    if (this->temperature_sensor_ != nullptr) {
        float temp = getTemperature();
        ESP_LOGD(TAG, "Temperature: %.1f°C", temp);
        this->temperature_sensor_->publish_state(temp);
    }
}

void AXP2101Component::SetSleep() {
    uint8_t reg26 = 0;
    readRegister(AXP2101_SLEEP_CFG, &reg26);
    reg26 |= (1 << 4);  // IRQ wake enable sicherheitshalber
    writeRegister(AXP2101_SLEEP_CFG, reg26);
}

void AXP2101Component::soft_power_off_esp() {
    ESP_LOGI(TAG, "Entering deep sleep (GPIO16 ext0 wakeup)");

    // Wait for POWER button release to avoid immediate ext0 wakeup
    int poll = 0;
    while (gpio_get_level(GPIO_NUM_16) == 0 && poll < 100) {
        delay_microseconds_safe(50000);  // 50ms
        poll++;
    }
    if (poll >= 100) {
        ESP_LOGI(TAG, "Button held too long, entering sleep anyway");
    }

    // Turn off backlight
    uint8_t ldo0 = 0;
    readRegister(AXP2101_LDO_ONOFF_CTRL0, &ldo0);
    ldo0 &= ~(1 << 7);
    writeRegister(AXP2101_LDO_ONOFF_CTRL0, ldo0);

    // Configure GPIO16 (POWER button) to wake from deep sleep
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_16, 0);  // wake when LOW

    ESP_LOGI(TAG, "Deep sleep: press POWER button to wake");
    delay_microseconds_safe(100000);
    esp_deep_sleep_start();
}

void AXP2101Component::power_off() {
    ESP_LOGW(TAG, "AXP2101 PMU soft power-off (0x10 bit0); press PWRON to wake");
    // Same as XPowersLib shutdown(): set COMMON_CONFIG bit0.
    if (!setBits(AXP2101_COMMON_CONFIG, 0x01)) {
        ESP_LOGE(TAG, "PMU power-off write failed, falling back to deep sleep");
        esp_deep_sleep_start();
        return;
    }
    // If the PMU does not cut the rails immediately, keep the device quiet
    // rather than running with a half-shut-down power tree.
    delay_microseconds_safe(500000);
    esp_deep_sleep_start();
}



void AXP2101Component::DeepSleep(uint64_t time_in_us) {
    // Diesen Pfad kannst du weiterverwenden, wenn du "normalen" ESP-Deep-Sleep willst,
    // bei dem der ESP noch versorgt bleibt.
    SetSleep();

    if (time_in_us > 0) {
        esp_sleep_enable_timer_wakeup(time_in_us);
    } else {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }
    esp_deep_sleep_start();  // hier bleibt die 3.3V Rail an!
}

void AXP2101Component::LightSleep(uint64_t time_in_us) {
    if (time_in_us > 0) {
        esp_sleep_enable_timer_wakeup(time_in_us);
    } else {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }
    
    esp_light_sleep_start();
}

std::string AXP2101Component::GetStartupReason() {
    esp_reset_reason_t reset_reason = esp_reset_reason();
    
    if (reset_reason == ESP_RST_DEEPSLEEP) {
        esp_sleep_source_t wake_reason = esp_sleep_get_wakeup_cause();
        switch (wake_reason) {
            case ESP_SLEEP_WAKEUP_EXT0: return "ESP_SLEEP_WAKEUP_EXT0";
            case ESP_SLEEP_WAKEUP_EXT1: return "ESP_SLEEP_WAKEUP_EXT1";
            case ESP_SLEEP_WAKEUP_TIMER: return "ESP_SLEEP_WAKEUP_TIMER";
            case ESP_SLEEP_WAKEUP_TOUCHPAD: return "ESP_SLEEP_WAKEUP_TOUCHPAD";
            case ESP_SLEEP_WAKEUP_ULP: return "ESP_SLEEP_WAKEUP_ULP";
            case ESP_SLEEP_WAKEUP_GPIO: return "ESP_SLEEP_WAKEUP_GPIO";
            case ESP_SLEEP_WAKEUP_UART: return "ESP_SLEEP_WAKEUP_UART";
            default: return "WAKEUP_UNKNOWN_REASON";
        }
    }
    
    switch (reset_reason) {
        case ESP_RST_UNKNOWN: return "ESP_RST_UNKNOWN";
        case ESP_RST_POWERON: return "ESP_RST_POWERON";
        case ESP_RST_SW: return "ESP_RST_SW";
        case ESP_RST_PANIC: return "ESP_RST_PANIC";
        case ESP_RST_INT_WDT: return "ESP_RST_INT_WDT";
        case ESP_RST_TASK_WDT: return "ESP_RST_TASK_WDT";
        case ESP_RST_WDT: return "ESP_RST_WDT";
        case ESP_RST_BROWNOUT: return "ESP_RST_BROWNOUT";
        case ESP_RST_SDIO: return "ESP_RST_SDIO";
        default: return "RESET_UNKNOWN_REASON";
    }
}

}  // namespace axp2101
}  // namespace esphome
