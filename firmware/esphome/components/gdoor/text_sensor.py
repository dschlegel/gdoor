import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv

from . import CONF_GDOOR_ID, GDOOR_CHILD_SCHEMA, gdoor_ns

DEPENDENCIES = ["gdoor"]

CONF_FIELD = "field"

GDoorTextSensor = gdoor_ns.class_(
    "GDoorTextSensor", text_sensor.TextSensor, cg.Component
)
GDoorTextSensorField = gdoor_ns.enum("GDoorTextSensorField")

FIELDS = {
    "busdata": GDoorTextSensorField.FIELD_BUSDATA,
    "action": GDoorTextSensorField.FIELD_ACTION,
    "type": GDoorTextSensorField.FIELD_TYPE,
    "source": GDoorTextSensorField.FIELD_SOURCE,
    "destination": GDoorTextSensorField.FIELD_DESTINATION,
    "parameters": GDoorTextSensorField.FIELD_PARAMETERS,
}

CONFIG_SCHEMA = (
    text_sensor.text_sensor_schema(GDoorTextSensor)
    .extend(GDOOR_CHILD_SCHEMA)
    .extend(
        {
            cv.Optional(CONF_FIELD, default="busdata"): cv.enum(FIELDS, lower=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_GDOOR_ID])
    cg.add(var.set_field(config[CONF_FIELD]))
