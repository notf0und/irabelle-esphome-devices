import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text
from esphome.const import CONF_ID, CONF_NAME, CONF_MODE

AUTO_LOAD = ["socket", "text"]
DEPENDENCIES = ["network"]

stream_microphone_ns = cg.esphome_ns.namespace("stream_microphone")
StreamMicrophone = stream_microphone_ns.class_("StreamMicrophone", cg.Component)
RestoreText = stream_microphone_ns.class_("RestoreText", text.Text)

CONF_IP_ADDRESS = "ip_address"
CONF_PORT = "port"
CONF_GAIN_FACTOR = "gain_factor"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(StreamMicrophone),
    cv.Optional(CONF_IP_ADDRESS, default="192.168.1.2"): cv.string,
    cv.Optional(CONF_PORT, default=5005): cv.port,
    cv.Optional(CONF_GAIN_FACTOR, default=1): cv.int_range(1, 64),
}).extend(cv.COMPONENT_SCHEMA)


def _build_text_config(parent_id, suffix, name, icon):
    id_str = f"{parent_id}_{suffix}"
    raw = {
        cv.GenerateID(): cv.declare_id(RestoreText)(id_str),
        cv.Required(CONF_NAME): name,
        cv.Required(CONF_MODE): "TEXT",
    }
    if icon:
        raw["icon"] = icon
    return text.text_schema(RestoreText, mode="TEXT")(raw)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    parent_id_str = str(config[CONF_ID])

    ip_conf = _build_text_config(parent_id_str, "ip", "Stream IP", "mdi:ip-network")
    ip_text = await text.new_text(ip_conf)
    cg.add(ip_text.set_initial_value(config[CONF_IP_ADDRESS]))
    cg.add(ip_text.set_parent(var))
    cg.add(var.set_ip_text(ip_text))

    port_conf = _build_text_config(parent_id_str, "port", "Stream Port", "mdi:numeric")
    port_text = await text.new_text(port_conf)
    cg.add(port_text.set_initial_value(str(config[CONF_PORT])))
    cg.add(port_text.set_parent(var))
    cg.add(var.set_port_text(port_text))

    gain_conf = _build_text_config(parent_id_str, "gain", "Stream Gain", "mdi:volume-high")
    gain_text = await text.new_text(gain_conf)
    cg.add(gain_text.set_initial_value(str(config[CONF_GAIN_FACTOR])))
    cg.add(gain_text.set_parent(var))
    cg.add(var.set_gain_text(gain_text))
