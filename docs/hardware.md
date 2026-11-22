# Hardware

## Seeed Studio XIAO ESP32-C6

esptool output from the development board (2026-11-05):

```
Chip type:  ESP32-C6FH4 (QFN32) (revision v0.2)
Features:   Wi-Fi 6, BT 5 (LE), IEEE802.15.4, Single Core + LP Core, 160MHz
USB mode:   USB-Serial/JTAG
Flash:      4 MB (in-package)
```

Pins (from the Seeed wiki and the arduino-esp32 `XIAO_ESP32C6` variant):

| Function | GPIO | Notes |
|---|---|---|
| RF switch enable | GPIO3 | Driven LOW to enable (`CONFIG_WB_RF_SWITCH_EN_GPIO`) |
| Antenna select | GPIO14 | LOW = on-board ceramic, HIGH = external U.FL (`CONFIG_WB_ANTENNA_EXTERNAL`) |
| User LED | GPIO15 | Active-low status LED |
| BOOT button | GPIO9 | Short press = join. Hold 10 s = network reset |
| On/Off output | GPIO0 | Follows the plug On/Off cluster (`CONFIG_WB_RELAY_GPIO`, -1 = none; `relay pin` at runtime) |
| USB-C | USB-Serial/JTAG | Console and flashing (VID 303A / PID 1001) |

For an external 2.4 GHz antenna on the U.FL connector, enable `CONFIG_WB_ANTENNA_EXTERNAL`
(`idf.py menuconfig` > WiserBridge Router > Board). Keep total EIRP within your regulatory limit;
default TX power is 18 dBm.

### Status LED

| Pattern | Meaning |
|---|---|
| Short blink every 2 s | Not commissioned (factory-new) |
| 5 Hz blink | Joining |
| Solid | Joined, with at least one router or hub neighbour |
| Double blink every 2 s | Joined, no router neighbour (isolated) |
| 1 Hz blink | Zigbee stack error (retrying) |
| 10 Hz blink | BOOT held long enough. Release to reset network membership |
| 2 Hz for about 30 s | Identify requested by the network |

### Power and placement

- Use a reliable mains USB supply. A brownout drops the router's children. It shows as
  `Reset reason: BROWNOUT` in the boot banner and is counted by `wb_health_report.py`.
- Place the C6 roughly mid-way between an existing router (for example the WA104) and the distant
  TRVs, with a good link to both.
- Avoid metal enclosures, radiators, spots behind TVs and, where practical, busy Wi-Fi access points.

## Other hardware

| Item | Role |
|---|---|
| Drayton Wiser Heat Hub | Zigbee coordinator and trust centre. Not modified. |
| Drayton Wiser WA104H1A0000 range extender | Existing router. Not modified, opened or reset. The joined C6 can interview it (see [protocol-investigation.md](protocol-investigation.md)). |
| Wiser TRVs | Sleepy end devices. Never unpaired by these tests. |
| Sonoff Zigbee Bridge (model unidentified) | Not required. The classic ZBBridge runs closed eWeLink firmware and cannot be a Zigbee2MQTT coordinator without reflashing; the Pro and dongle variants differ. |
| 3 × ESP32-C3 | Not usable: no IEEE 802.15.4 radio. |

A bench scan found four nearby Zigbee networks (channels 11, 16, 20, 24). Use `epid` and `channels`
so the C6 joins only the Wiser network.

## Optional: sniffer

Sniffing while the C6 routes needs a second IEEE 802.15.4 radio: another ESP32-C6 or an ESP32-H2
running `sniffer/`, or a sniffer-capable dongle. An ESP32-C3 cannot do this.
