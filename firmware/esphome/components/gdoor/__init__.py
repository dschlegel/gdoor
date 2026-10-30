import re

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import esp32, output
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_OUTPUT,
    CONF_RX_PIN,
    CONF_TRIGGER_ID,
    CONF_TX_PIN,
    CONF_VOLTAGE,
)

CODEOWNERS = ["@gdoor-org"]
DEPENDENCIES = ["esp32"]
MULTI_CONF = True

CONF_GDOOR_ID = "gdoor_id"
CONF_TX_ENABLE_PIN = "tx_enable_pin"
CONF_RX_THRESHOLD = "rx_threshold"
CONF_ON_MESSAGE = "on_message"
CONF_BUSDATA = "busdata"

MAX_WORDLEN = 25

gdoor_ns = cg.esphome_ns.namespace("gdoor")
GDoorComponent = gdoor_ns.class_("GDoorComponent", cg.Component)
GDoorMessage = gdoor_ns.struct("GDoorMessage")
GDoorMessageConstRef = GDoorMessage.operator("const").operator("ref")
MessageTrigger = gdoor_ns.class_(
    "MessageTrigger", automation.Trigger.template(GDoorMessageConstRef)
)
SendAction = gdoor_ns.class_("SendAction", automation.Action, cg.Parented.template(GDoorComponent))


def hex_bytes(length=None, max_length=None):
    """Validate a hex string like "A286FD", returns it in upper case."""

    def validator(value):
        value = cv.string_strict(value).replace(" ", "").replace(":", "").upper()
        if not re.fullmatch(r"([0-9A-F]{2})+", value):
            raise cv.Invalid(f"'{value}' is not a hex string like 'A286FD'")
        if length is not None and len(value) != length * 2:
            raise cv.Invalid(f"Needs exactly {length} bytes ({length * 2} hex characters)")
        if max_length is not None and len(value) > max_length * 2:
            raise cv.Invalid(f"At most {max_length} bytes ({max_length * 2} hex characters)")
        return value

    return validator


# Bus data to send, without checksum, which is added on sending
validate_send_busdata = hex_bytes(max_length=MAX_WORDLEN - 1)

GDOOR_CHILD_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_GDOOR_ID): cv.use_id(GDoorComponent),
    }
)

# Defaults match the GDoor v3.1 hardware
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(GDoorComponent),
            cv.Optional(CONF_RX_PIN, default="GPIO22"): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_TX_PIN, default="GPIO25"): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_TX_ENABLE_PIN, default="GPIO27"): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_RX_THRESHOLD): cv.Schema(
                {
                    cv.Required(CONF_OUTPUT): cv.use_id(output.FloatOutput),
                    cv.Optional(CONF_VOLTAGE, default="1.45V"): cv.All(
                        cv.voltage, cv.Range(min=0.0, max=3.3)
                    ),
                }
            ),
            cv.Optional(CONF_ON_MESSAGE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(MessageTrigger),
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


async def to_code(config):
    # RX uses the GPIO interrupt only, TX the RMT peripheral which creates the carrier in hardware
    esp32.include_builtin_idf_component("esp_driver_rmt")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_rx_pin(await cg.gpio_pin_expression(config[CONF_RX_PIN])))
    cg.add(var.set_tx_pin(await cg.gpio_pin_expression(config[CONF_TX_PIN])))
    cg.add(var.set_tx_enable_pin(await cg.gpio_pin_expression(config[CONF_TX_ENABLE_PIN])))

    if threshold := config.get(CONF_RX_THRESHOLD):
        out = await cg.get_variable(threshold[CONF_OUTPUT])
        cg.add(var.set_rx_threshold(out, threshold[CONF_VOLTAGE]))

    for conf in config.get(CONF_ON_MESSAGE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(GDoorMessageConstRef, "x")], conf
        )


@automation.register_action(
    "gdoor.send",
    SendAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(GDoorComponent),
            cv.Required(CONF_BUSDATA): cv.templatable(validate_send_busdata),
        },
        key=CONF_BUSDATA,
    ),
    synchronous=True,
)
async def send_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    busdata = await cg.templatable(config[CONF_BUSDATA], args, cg.std_string)
    cg.add(var.set_busdata(busdata))
    return var
