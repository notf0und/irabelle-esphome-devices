import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID

from . import AXP2101Component, axp2101_ns

DEPENDENCIES = ["axp2101"]

AXP2101ChargingSwitch = axp2101_ns.class_(
    "AXP2101ChargingSwitch", switch.Switch, cg.Component
)

CONF_AXP2101_ID = "axp2101_id"
CONF_CHARGING_ENABLED = "charging_enabled"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_AXP2101_ID): cv.use_id(AXP2101Component),
        cv.Optional(CONF_CHARGING_ENABLED): switch.switch_schema(
            AXP2101ChargingSwitch,
            default_restore_mode="RESTORE_DEFAULT_ON",
            icon="mdi:battery-charging",
        ).extend(cv.COMPONENT_SCHEMA),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_AXP2101_ID])

    if conf := config.get(CONF_CHARGING_ENABLED):
        sw = await switch.new_switch(conf)
        await cg.register_component(sw, conf)
        cg.add(sw.set_parent(parent))
        cg.add(parent.set_charging_switch(sw))
