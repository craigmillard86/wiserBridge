# Hardware

## Seeed Studio XIAO ESP32-C6 (verified on the bench)

The board used during development reported (esptool, 2026-11-05):

```
Chip type:  ESP32-C6FH4 (QFN32) (revision v0.2)
Features:   Wi-Fi 6, BT 5 (LE), IEEE802.15.4, Single Core + LP Core, 160MHz
USB mode:   USB-Serial/JTAG
Flash:      4 MB (in-package)
```

Board details used by the firmware (from the Seeed wiki and the arduino-esp32 `XIAO_ESP32C6` variant
files, cross-checked on hardware where noted):

| Function | GPIO | Notes |
|---|---|---|
| RF switch enable | GPIO3 | Driven **LOW** to enable the RF switch (`CONFIG_WB_RF_SWITCH_EN_GPIO`) |
| Antenna select | GPIO14 | **LOW** = on-board ceramic antenna, **HIGH** = external U.FL (`CONFIG_WB_ANTENNA_EXTERNAL`) |
| User LED | GPIO15 | Active-low status LED |
| BOOT button | GPIO9 | Short press = join, hold 10 s = network reset |
| USB-C | native USB-Serial/JTAG | Console + flashing. Windows enumerates VID 303A / PID 1001 |

The firmware sets the RF switch before the radio starts. If you fit an external 2.4 GHz antenna on
the U.FL connector, enable `CONFIG_WB_ANTENNA_EXTERNAL` (`idf.py menuconfig` → WiserBridge Router →
Board) and keep the total EIRP within your regulatory limit (the default TX power is 18 dBm).

### Status LED

| Pattern | Meaning |
|---|---|
| short blink every 2 s | not commissioned (factory-new) |
| fast 5 Hz blink | joining (steering window open) |
| solid on | joined, at least one router/hub neighbour |
| double blink every 2 s | joined but **no router neighbour** (isolated) |
| 1 Hz blink | Zigbee stack error (retrying) |
| very fast 10 Hz | BOOT held long enough — release to reset network membership |
| 2 Hz for ~30 s | Identify requested by the network |

### Power and placement

* Use a reliable mains USB power supply. A router that browns out drops its children; a brownout is
  shown as `Reset reason: BROWNOUT` in the boot banner and counted by `wb_health_report.py`.
* Place the C6 where it has a **good link to an existing router** (e.g. the WA104) *and* to the
  distant TRVs — roughly mid-way. Avoid metal enclosures, radiators and placement directly behind TVs.
* Keep it away from busy Wi-Fi access points where practical (2.4 GHz overlap with the Zigbee channel).

## Other hardware in this project

| Item | Role here |
|---|---|
| Drayton Wiser Heat Hub | Zigbee coordinator and trust centre. Not modified. |
| Drayton Wiser WA104H1A0000 range extender | Existing Zigbee router. **Not modified, not opened, not factory-reset.** Can be interviewed over the air by the C6 once the C6 has joined (see protocol-investigation.md). |
| Wiser TRVs | Battery-powered sleepy end devices. Never unpaired by these tests. |
| Sonoff Zigbee Bridge (model unidentified) | Not required. The classic Sonoff ZBBridge runs closed eWeLink firmware and cannot act as an independent Zigbee2MQTT coordinator without reflashing; the "Bridge Pro" / dongle variants differ. Identify the model before planning to use it as a bench coordinator. |
| 3 × ESP32-C3 | **No IEEE 802.15.4 radio** — cannot be Zigbee routers or sniffers. Possible uses: none required for this project. |

Nearby networks: a bench scan from the development PC saw four Zigbee PANs (channels 11, 16, 20, 24).
Use `epid`/`channels` so the C6 can only join the Wiser one.

## Optional extra hardware (only if you need independent proof)

A **second** IEEE 802.15.4 radio is needed to sniff while the C6 routes — e.g. another ESP32-C6 or
ESP32-H2 running `sniffer/`, or another sniffer-capable dongle. An ESP32-C3 cannot do this.
