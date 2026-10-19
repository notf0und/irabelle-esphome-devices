"""On-device internet radio: station directory (radio-browser.info) + favourites.

The component owns the station data, the favourites list (persisted in NVS), the
radio-browser query/response handling and the HTTP GET itself; the YAML side owns
the LVGL UI and the triggers. The GET lives in C++ (not the shared http_request
component) so the response body can be buffered in PSRAM - internal DRAM on this
board is down to a few tens of kB and http_request's internal std::string body
used to abort()/rollback the device.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import media_player
from esphome.const import CONF_ID

CODEOWNERS = ["@gonzalo"]
DEPENDENCIES = ["media_player", "watchdog"]
AUTO_LOAD = ["json"]

CONF_MEDIA_PLAYER = "media_player"
CONF_MAX_RESULTS = "max_results"
CONF_ON_UPDATED = "on_updated"
CONF_ON_HA_PLAY = "on_ha_play"
CONF_ON_PREFETCH = "on_prefetch"

sh_radio_ns = cg.esphome_ns.namespace("sh_radio")
ShRadio = sh_radio_ns.class_("ShRadio", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ShRadio),
        cv.Required(CONF_MEDIA_PLAYER): cv.use_id(media_player.MediaPlayer),
        cv.Optional(CONF_MAX_RESULTS, default=20): cv.int_range(min=1, max=50),
        cv.Optional(CONF_ON_UPDATED): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_HA_PLAY): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_PREFETCH): automation.validate_automation(single=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    mp = await cg.get_variable(config[CONF_MEDIA_PLAYER])
    cg.add(var.set_media_player(mp))
    cg.add(var.set_max_results(config[CONF_MAX_RESULTS]))

    if on_updated := config.get(CONF_ON_UPDATED):
        await automation.build_automation(var.get_updated_trigger(), [], on_updated)

    if on_ha_play := config.get(CONF_ON_HA_PLAY):
        await automation.build_automation(var.get_ha_play_trigger(), [], on_ha_play)

    if on_prefetch := config.get(CONF_ON_PREFETCH):
        await automation.build_automation(var.get_prefetch_trigger(), [], on_prefetch)
