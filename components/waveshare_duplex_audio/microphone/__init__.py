import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_BITS_PER_SAMPLE, CONF_NUM_CHANNELS, CONF_SAMPLE_RATE
from esphome.components import audio, microphone
from esphome.components.microphone import register_microphone

from .. import waveshare_duplex_audio_ns, WaveshareDuplexAudio

DEPENDENCIES = ["waveshare_duplex_audio"]
AUTO_LOAD = ["microphone"]

CONF_WAVESHARE_DUPLEX_AUDIO_ID = "waveshare_duplex_audio_id"
CONF_AEC_GAIN = "aec_gain"
CONF_AEC_NLP = "aec_nlp"

NLP_LEVELS = {
    "normal": 0,
    "aggressive": 1,
    "very_aggressive": 2,
}

WaveshareDuplexMicrophone = waveshare_duplex_audio_ns.class_(
    "WaveshareDuplexMicrophone",
    microphone.Microphone,
    cg.Component,
)


def _set_stream_limits(config):
    audio.set_stream_limits(
        min_bits_per_sample=config.get(CONF_BITS_PER_SAMPLE),
        max_bits_per_sample=config.get(CONF_BITS_PER_SAMPLE),
        min_channels=config.get(CONF_NUM_CHANNELS),
        max_channels=config.get(CONF_NUM_CHANNELS),
        min_sample_rate=config.get(CONF_SAMPLE_RATE),
        max_sample_rate=config.get(CONF_SAMPLE_RATE),
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    microphone.MICROPHONE_SCHEMA.extend({
        cv.GenerateID(): cv.declare_id(WaveshareDuplexMicrophone),
        cv.GenerateID(CONF_WAVESHARE_DUPLEX_AUDIO_ID): cv.use_id(WaveshareDuplexAudio),
        cv.Optional(CONF_BITS_PER_SAMPLE, default=16): cv.int_range(16, 16),
        cv.Optional(CONF_NUM_CHANNELS, default=1): cv.int_range(1, 1),
        cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(8000, 48000),
        cv.Optional(CONF_AEC_GAIN, default=5.0): cv.float_range(0.0, 100.0),
        cv.Optional(CONF_AEC_NLP, default="aggressive"): cv.one_of(*NLP_LEVELS),
    }).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await register_microphone(var, config)

    parent = await cg.get_variable(config[CONF_WAVESHARE_DUPLEX_AUDIO_ID])
    cg.add(var.set_parent(parent))
    # Voice-stack rate (16 kHz). The I2S bus may run faster; the mic task
    # decimates down to this rate before anything downstream sees it.
    cg.add(var.set_output_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_aec_gain(config[CONF_AEC_GAIN]))
    cg.add(var.set_aec_nlp(NLP_LEVELS[config[CONF_AEC_NLP]]))
