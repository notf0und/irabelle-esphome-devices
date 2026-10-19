"""
Waveshare Duplex Audio component.

Creates a single I2S_NUM_0 duplex channel at boot with:
  - TX (speaker):  standard I2S mode → ES8311 DAC
  - RX (microphone): TDM mode with 4 slots → ES7210 ADC

The ES7210 TDM slot order is MIC1, MIC3, MIC2, MIC4 (not the physical
numbering), so slot 0 = the board microphone and slot 1 = the hardware
echo-cancellation reference that the board feeds from the speaker amp
through the on-board divider network.

Both channels are enabled immediately so the microphone clock is always
present, fixing the "wake word only works after media player plays" issue.
"""

import os

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_SAMPLE_RATE
from esphome.components import i2c

CODEOWNERS = []
MULTI_CONF = True
DEPENDENCIES = ["i2c"]

waveshare_duplex_audio_ns = cg.esphome_ns.namespace("waveshare_duplex_audio")
WaveshareDuplexAudio = waveshare_duplex_audio_ns.class_(
    "WaveshareDuplexAudio", cg.Component, i2c.I2CDevice
)

CONF_MCLK_PIN = "mclk_pin"
CONF_BCLK_PIN = "bclk_pin"
CONF_LRCK_PIN = "lrck_pin"
CONF_DOUT_PIN = "dout_pin"
CONF_DIN_PIN  = "din_pin"
CONF_PA_PIN   = "pa_pin"

CONFIG_SCHEMA = (
    cv.Schema({
        cv.GenerateID(): cv.declare_id(WaveshareDuplexAudio),
        cv.Required(CONF_MCLK_PIN): cv.positive_int,
        cv.Required(CONF_BCLK_PIN): cv.positive_int,
        cv.Required(CONF_LRCK_PIN): cv.positive_int,
        cv.Required(CONF_DOUT_PIN): cv.positive_int,
        cv.Required(CONF_DIN_PIN):  cv.positive_int,
        cv.Optional(CONF_PA_PIN, default=-1): cv.int_,
        cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(min=8000, max=48000),
    })
    .extend(cv.COMPONENT_SCHEMA)
    # Adds CONF_ADDRESS (default 0x40 = ES7210) and CONF_I2C_ID. The I2C
    # handle is only used to put the ADC into TDM mode at mic-task start.
    .extend(i2c.i2c_device_schema(0x40))
)


# scripts/pack-models.sh --emit-blob drops the packed ESP-SR models next to
# this component; compiling that file in makes the mic front-end able to load
# them with srmodel_load() when no "model" partition exists.
_EMBEDDED_BLOB = os.path.join(os.path.dirname(os.path.realpath(__file__)), "srmodels_blob.cpp")


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    if os.path.exists(_EMBEDDED_BLOB):
        cg.add_define("USE_SRMODELS_BLOB")

    cg.add(var.set_mclk_pin(config[CONF_MCLK_PIN]))
    cg.add(var.set_bclk_pin(config[CONF_BCLK_PIN]))
    cg.add(var.set_lrck_pin(config[CONF_LRCK_PIN]))
    cg.add(var.set_dout_pin(config[CONF_DOUT_PIN]))
    cg.add(var.set_din_pin(config[CONF_DIN_PIN]))
    cg.add(var.set_pa_pin(config[CONF_PA_PIN]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
