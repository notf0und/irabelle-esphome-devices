"""LVGL Screenshot Component - serves a JPEG snapshot of the LVGL framebuffer via HTTP."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT

CODEOWNERS = ["@dcgrove"]
DEPENDENCIES = ["lvgl"]

lvgl_ns = cg.esphome_ns.namespace("lvgl")
LvglComponent = lvgl_ns.class_("LvglComponent", cg.Component)

lvgl_screenshot_ns = cg.esphome_ns.namespace("lvgl_screenshot")
LvglScreenshot = lvgl_screenshot_ns.class_("LvglScreenshot", cg.Component)

CONF_LVGL_ID = "lvgl_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LvglScreenshot),
        cv.Optional(CONF_PORT, default=8080): cv.port,
        cv.Optional(CONF_LVGL_ID): cv.use_id(LvglComponent),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_port(config[CONF_PORT]))
    if CONF_LVGL_ID in config:
        lvgl = await cg.get_variable(config[CONF_LVGL_ID])
        cg.add(var.set_lvgl(lvgl))
