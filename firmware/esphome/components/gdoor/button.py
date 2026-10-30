import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv

from . import (
    CONF_BUSDATA,
    CONF_GDOOR_ID,
    GDOOR_CHILD_SCHEMA,
    GDoorComponent,
    gdoor_ns,
    validate_send_busdata,
)

DEPENDENCIES = ["gdoor"]

GDoorButton = gdoor_ns.class_(
    "GDoorButton", button.Button, cg.Parented.template(GDoorComponent)
)

CONFIG_SCHEMA = (
    button.button_schema(GDoorButton)
    .extend(GDOOR_CHILD_SCHEMA)
    .extend(
        {
            # Bus data to send, without checksum
            cv.Required(CONF_BUSDATA): validate_send_busdata,
        }
    )
)


async def to_code(config):
    var = await button.new_button(config)
    await cg.register_parented(var, config[CONF_GDOOR_ID])
    cg.add(var.set_busdata(config[CONF_BUSDATA]))
