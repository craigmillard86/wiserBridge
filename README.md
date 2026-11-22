# WiserBridge

Firmware that turns an ESP32-C6 board into a Zigbee router for a Drayton Wiser heating system. Put
one between the Wiser hub and radiator thermostats (TRVs) that keep dropping off, and the TRVs can
route through it.

```
Wiser Heat Hub ──► ESP32-C6 running WiserBridge ──► distant Wiser TRVs
```

It joins your existing Wiser network as an ordinary router and shows up in the Wiser app as a smart
plug. It never creates its own network. Wi-Fi and Bluetooth stay off.

This is not a Drayton product. It works by presenting the identity of the Wiser smart plug (a Heiman
SmartPlug-EF-3.0), because the hub removes devices it doesn't recognise. A future hub update could
change that.

Status: two boards have been running on a real Wiser network since 2026-11-06, with four TRVs routed
through one of them. See [docs/test-results.md](docs/test-results.md).

## Install from your browser

The quickest way: open **https://craigmillard86.github.io/wiserBridge/** in Chrome or Edge on a
desktop computer.

1. Pick your board (XIAO or another ESP32-C6) and click Install. Choose the board's
   port from the list the browser shows. Erase when asked on a new board.
2. Power the board from a USB charger where it will live (see step 5).
3. Start adding a device in the Wiser app so the hub opens pairing.
4. **Press the BOOT button on the board once.** After a minute a new smart plug appears in the app.

No computer needed after flashing. The Console page has a Join button, status and
troubleshooting if you want to watch it or something goes wrong. Nothing to install on your
computer. The rest of this page is for building
it yourself.

## What you need

- A Seeed Studio XIAO ESP32-C6 (tested). Other ESP32-C6 boards work, such as the SuperMini (also
  tested); see [Other boards](#other-boards).
- A USB-C data cable and a USB power supply for where the board will live.
- To build yourself: a PC with Python 3 and Docker Desktop. Not needed for the browser install.
- The Wiser app, to open pairing on the hub.

## 1. Build

From WSL or Git Bash, with Docker Desktop running:

```
tools/build.sh
```

This builds the router and sniffer firmware with ESP-IDF v5.5.6 and writes flash images to
`release/`. To build without Docker or change settings, see [docs/building.md](docs/building.md).

## 2. Flash

Plug the board in. It shows up as "USB Serial Device (COMx)". Replace `COM8` with your port.

```
py -m pip install --upgrade esptool pyserial
py -m esptool --chip esp32c6 -p COM8 erase-flash
py -m esptool --chip esp32c6 -p COM8 write-flash 0x0 release\wiserbridge-router-0.1.0-xiao-esp32c6.bin
```

More detail: [docs/flashing.md](docs/flashing.md).

## 3. Open the console

```
py tools\wb_monitor.py --port COM8 --duration 0
```

Type `help` for the command list. Opening the port restarts the board; that's expected.

## 4. Join

Start adding a device in the Wiser app so the hub opens pairing. Then **press the BOOT button on the
board once** (a short press), or type `join` on the console. The board doesn't need to be connected
to a computer for the button. The LED blinks fast while joining and stays on when joined (XIAO).

After a minute a new smart plug appears in the app. On the console, run `status`. `JOINED_LINKS_PRESENT`,
`JOINED_COORDINATOR_REACHABLE` or `JOINED_MULTIHOP_VERIFIED` means it worked, and a new smart plug
appears in the app.

If you see `JOINED_THEN_REMOVED`, the hub removed it. Run `profile show`: it should say `HEIMAN` /
`SmartPlug-EF-3.0`. If it says `DIY-ESP32C6` / `ZB-ROUTER-C6`, the board has an old saved profile.
Run `profile defaults`, `profile save`, `reboot`, then join again. Other verdicts are explained in
[docs/troubleshooting.md](docs/troubleshooting.md).

**Optional: lock it to your network.** The board joins whichever Zigbee network has pairing open.
If a neighbour might be pairing a Zigbee device at the same time, lock it to yours first: run
`scan`, open pairing in the Wiser app, run `scan` again, and run `epid <EPID>` with the EPID of the
network that changed to `OPEN`.

## 5. Place it

Unplug the board and power it from a USB charger somewhere between the hub and the far TRVs. It keeps
its network membership across power cuts.

Put it where it has a good link to the hub or to another router, not at the edge of range. A board
that barely hears the hub can lose its route.

TRVs only switch to a new parent when they rejoin. To move them onto the new router sooner, power-cycle
the router they currently use (for example a Wiser smart plug) for a minute or two.

To check it's working, plug it into the PC and run `status`, `children` and `ping 0x0000`. `children`
lists the TRVs using it.

## Status LED (XIAO)

| Pattern | Meaning |
|---|---|
| Short blink every 2 s | Not joined |
| Fast blink | Joining |
| Solid | Joined |
| Double blink every 2 s | Joined, but no other router nearby |
| 1 Hz blink | Zigbee stack error, retrying |

To leave the network and clear it, hold BOOT for 10 s and let go when the LED starts blinking very
fast. The console `reset` command does the same.

## Other boards

The default build is for the XIAO, which has an antenna switch on GPIO3 and GPIO14. For other ESP32-C6
boards, run `idf.py menuconfig` and under WiserBridge Router → Board set the RF switch and antenna
select GPIOs to -1, and set the LED GPIO for your board. If you fit an external antenna to a XIAO,
enable `CONFIG_WB_ANTENNA_EXTERNAL`. See [docs/hardware.md](docs/hardware.md).

## Limitations

- The hub only keeps devices it recognises. WiserBridge borrows the Wiser smart plug identity.
- The Wiser app shows it as a plug. Its On/Off switch drives GPIO0.
- Routes to the hub can fade at the edge of range, and the firmware can't force a route.
- A TRV close to the board won't necessarily use it.
- If the hub changes its network key while the board is off, reset and rejoin it.

## More docs

- [Hardware](docs/hardware.md), [building](docs/building.md), [flashing](docs/flashing.md)
- [Joining the Wiser hub](docs/commissioning.md) and [console commands](docs/cli-reference.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Test results](docs/test-results.md) and [test procedures](docs/multihop-testing.md)
- [Investigation log](docs/wiser-public-sources.md), [identity and sniffing](docs/protocol-investigation.md),
  [v0.1.0 bench report](docs/verification-report.md), [Wi-Fi bridge notes](docs/wifi-bridge-feasibility.md),
  [Home Assistant map card](docs/ha-wiser-map.md)
- Source layout: `main/` router firmware, `sniffer/` capture firmware, `tools/` build and analysis
  scripts, `tests/` host tests (`tests/run_all.sh`)

License: Apache-2.0.
