# GDoor ESPHome component

ESPHome external component for the GDoor hardware, as an alternative to the MQTT firmware in `firmware/esp32`.
ESPHome takes care of WiFi, OTA updates and the Home Assistant connection, the component only talks to the bus.

See [gdoor.yaml](gdoor.yaml) for a complete example.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/gdoor-org/gdoor
      path: firmware/esphome/components
    components: [gdoor]

output:
  - platform: esp32_dac
    pin: GPIO26
    id: gdoor_rx_threshold

gdoor:
  rx_pin: GPIO22
  rx_threshold:
    output: gdoor_rx_threshold
    voltage: 1.45V
```

## Configuration

| Option          | Default  | Description |
|-----------------|----------|-------------|
| `rx_pin`        | `GPIO22` | RX input. v3.1 adjustable: `GPIO22`, v3.1: `GPIO21`, v3.0 bugfix: `GPIO12`, v3.0: `GPIO32` |
| `tx_pin`        | `GPIO25` | TX carrier output |
| `tx_enable_pin` | `GPIO27` | TX output buffer enable |
| `rx_threshold`  |          | Only for `GPIO22`: `output` (an `esp32_dac` output on `GPIO26`) and `voltage` (Low: `1.3V`, Med: `1.45V`, High: `1.65V`) |
| `on_message`    |          | Automation, triggered for every valid bus message. The message is available as `x` |

The message fields are upper case hex strings, except `action` and `type`:

| Field         | Example        | Description |
|---------------|----------------|-------------|
| `busdata`     | `0211...`      | Complete message incl. checksum |
| `action`      | `BUTTON_RING`  | See the action list on the [GDoor homepage](https://gdoor-org.github.io/) |
| `type`        | `OUTDOOR`      | Hardware type of the sender |
| `source`      | `A286FD`       | Address of the sender |
| `destination` | `000000`       | Address of the receiver |
| `parameters`  | `0360`         | e.g. which button was pressed |

Every received message is also logged, so the log shows the values you need for the entities below.

## Entities

```yaml
# On for auto_off (default 1s) when a matching message is received.
# Any combination of busdata, action, source, destination, parameters.
# busdata (incl. checksum) can also be a list, it then matches any of them.
binary_sensor:
  - platform: gdoor
    name: "Door bell"
    action: BUTTON_RING
    parameters: "0360"

# Sends busdata (without checksum, it is added automatically), e.g. to open the door.
# It is the same hex string as sent via MQTT with the MQTT firmware.
button:
  - platform: gdoor
    name: "Open door"
    busdata: "..."

# Publishes a field of every received message: busdata, action, type, source, destination or parameters.
text_sensor:
  - platform: gdoor
    name: "Last bus message"
    field: busdata
```

## Actions

```yaml
- gdoor.send: "0211..."
- gdoor.send:
    busdata: !lambda 'return "0211...";'
```

## Implementation notes

- RX only uses the GPIO interrupt: it counts the carrier edges per bit and detects the end of a bit
  or message by the time since the last edge. No hardware timers are needed.
- TX uses the RMT peripheral, which creates the 52kHz carrier and the bit timing in hardware.
- Messages are only sent while the bus is idle, up to 5 messages are queued.
