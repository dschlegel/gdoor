import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import CONF_ACTION, CONF_PARAMETERS, CONF_SOURCE

from . import CONF_BUSDATA, CONF_GDOOR_ID, GDOOR_CHILD_SCHEMA, gdoor_ns, hex_bytes

DEPENDENCIES = ["gdoor"]

CONF_DESTINATION = "destination"
CONF_AUTO_OFF = "auto_off"

GDoorBinarySensor = gdoor_ns.class_(
    "GDoorBinarySensor", binary_sensor.BinarySensor, cg.Component
)

FILTERS = [CONF_BUSDATA, CONF_ACTION, CONF_SOURCE, CONF_DESTINATION, CONF_PARAMETERS]

CONFIG_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema(GDoorBinarySensor)
    .extend(GDOOR_CHILD_SCHEMA)
    .extend(
        {
            # Received busdata incl. checksum, matches any of the list
            cv.Optional(CONF_BUSDATA): cv.ensure_list(hex_bytes()),
            cv.Optional(CONF_ACTION): cv.All(cv.string_strict, cv.Upper),
            cv.Optional(CONF_SOURCE): hex_bytes(length=3),
            cv.Optional(CONF_DESTINATION): hex_bytes(length=3),
            cv.Optional(CONF_PARAMETERS): hex_bytes(length=2),
            cv.Optional(
                CONF_AUTO_OFF, default="1s"
            ): cv.positive_time_period_milliseconds,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    cv.has_at_least_one_key(*FILTERS),
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_GDOOR_ID])

    for busdata in config.get(CONF_BUSDATA, []):
        cg.add(var.add_busdata(busdata))
    for key in FILTERS[1:]:
        if key in config:
            cg.add(getattr(var, f"set_{key}")(config[key]))
    cg.add(var.set_auto_off(config[CONF_AUTO_OFF]))
