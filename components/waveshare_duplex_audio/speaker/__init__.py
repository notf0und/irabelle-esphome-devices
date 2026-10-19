"""
Speaker platform for waveshare_duplex_audio.

Uses the pre-created TX handle (standard I2S mode) from the parent component.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_BITS_PER_SAMPLE, CONF_NUM_CHANNELS, CONF_SAMPLE_RATE
from esphome.components import audio, speaker
from esphome.components.speaker import register_speaker

# Optional audio_dac for volume control
CONF_AUDIO_DAC = "audio_dac"

try:
    from esphome.components.audio_dac import AudioDac
    HAS_AUDIO_DAC = True
except ImportError:
    HAS_AUDIO_DAC = False

from .. import waveshare_duplex_audio_ns, WaveshareDuplexAudio

DEPENDENCIES = ["waveshare_duplex_audio"]
AUTO_LOAD = ["speaker"]

CONF_WAVESHARE_DUPLEX_AUDIO_ID = "waveshare_duplex_audio_id"
CONF_BUFFER_DURATION = "buffer_duration"
CONF_TIMEOUT = "timeout"

WaveshareDuplexSpeaker = waveshare_duplex_audio_ns.class_(
    "WaveshareDuplexSpeaker",
    speaker.Speaker,
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


_BASE_SCHEMA = speaker.SPEAKER_SCHEMA.extend({
    cv.GenerateID(): cv.declare_id(WaveshareDuplexSpeaker),
    cv.GenerateID(CONF_WAVESHARE_DUPLEX_AUDIO_ID): cv.use_id(WaveshareDuplexAudio),
    cv.Optional(CONF_BUFFER_DURATION, default="500ms"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_TIMEOUT): cv.positive_time_period_milliseconds,
    # Required by mixer's inherit_property_from — must not be None
    cv.Optional(CONF_NUM_CHANNELS, default=2): cv.int_range(min=1, max=2),
    cv.Optional(CONF_BITS_PER_SAMPLE, default=16): cv.int_range(16, 16),
    cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(min=8000, max=48000),
}).extend(cv.COMPONENT_SCHEMA)

# Add optional audio_dac field if available
if HAS_AUDIO_DAC:
    _BASE_SCHEMA = _BASE_SCHEMA.extend({
        cv.Optional(CONF_AUDIO_DAC): cv.use_id(AudioDac),
    })

CONFIG_SCHEMA = cv.All(_BASE_SCHEMA, _set_stream_limits)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await register_speaker(var, config)

    parent = await cg.get_variable(config[CONF_WAVESHARE_DUPLEX_AUDIO_ID])
    cg.add(var.set_parent(parent))

    cg.add(var.set_buffer_duration(config[CONF_BUFFER_DURATION]))
    if CONF_TIMEOUT in config:
        cg.add(var.set_timeout(config[CONF_TIMEOUT]))

    if HAS_AUDIO_DAC and CONF_AUDIO_DAC in config:
        dac = await cg.get_variable(config[CONF_AUDIO_DAC])
        cg.add(var.set_audio_dac(dac))
