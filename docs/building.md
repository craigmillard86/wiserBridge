# Building

## Versions

| Component | Version |
|---|---|
| ESP-IDF | v5.5.6 |
| esp-zigbee-lib | 1.6.8 (`~1.6.8`) |
| esp-zboss-lib | 1.6.4 (`~1.6.4`) |

esp-zigbee-lib 1.6.x is the ZBOSS-based v1.x LTS stack. The project stays on it rather than the
newer v2.x stack because v1.x has the longer field record. Dependencies in `main/idf_component.yml`
download automatically.

## Option A: Docker

Requires Docker Desktop with WSL integration enabled.

```bash
cd /mnt/c/Users/<you>/Documents/GIT/WiserBridge
tools/build.sh
```

This builds the router (`build/`), the sniffer (`sniffer/build/`), and merged images with
`SHA256SUMS.txt` in `release/`. The manual equivalent for the router:

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5.6 idf.py set-target esp32c6
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5.6 idf.py build
```

## Option B: native ESP-IDF on Windows

1. Install ESP-IDF v5.5.6 with the Espressif "ESP-IDF Tools Installer" (target esp32c6).
2. Open the "ESP-IDF 5.5 PowerShell" (or CMD) shortcut.
3. Build and flash. Replace COM8 with your port (see [flashing.md](flashing.md)).

   ```powershell
   cd C:\Users\<you>\Documents\GIT\WiserBridge
   idf.py set-target esp32c6
   idf.py build
   idf.py -p COM8 flash monitor
   ```

4. For the sniffer, run `idf.py set-target esp32c6` and `idf.py build` in `sniffer\`.

## Configuration

Run `idf.py menuconfig` and open **WiserBridge Router**:

- Device identity: manufacturer, model, date code, endpoint, profile and device ID, device
  version, node manufacturer code, Identify cluster, server and client cluster lists.
- Zigbee network: max children (20), network size (64), join channel mask, TCLK exchange, TX power
  (18 dBm), join window, auto-join (off), isolation alert.
- Diagnostics: HEALTH line interval (300 s).
- Board: RF switch, antenna, LED, button and On/Off output GPIOs, reset hold time.

The default identity is the Wiser smart plug:

| Option | Default |
|---|---|
| `WB_MANUFACTURER_NAME` | `HEIMAN` |
| `WB_MODEL_IDENTIFIER` | `SmartPlug-EF-3.0` |
| `WB_DATE_CODE` | `20231122` |
| `WB_DEVICE_ID` | 0x0051 (Smart Plug) |
| `WB_DEVICE_VERSION` | 1 |
| `WB_NODE_MANUFACTURER_CODE` | 0x120B |
| `WB_IN_CLUSTERS` (extra server clusters) | `0x0004,0x0005,0x0006,0x0009,0x0702,0x0b04,0x0b05` |
| `WB_OUT_CLUSTERS` (client clusters) | `0x0003,0x0019` |

Do not use a generic identity. The hub removes it about 10 s after joining. The old default
(`DIY-ESP32C6` / `ZB-ROUTER-C6`, device 0x0008) is one. Most identity fields can also be changed at
runtime with `profile set`.

An existing `sdkconfig` keeps its old values. `tools/build.sh` runs `set-target`, which regenerates
`sdkconfig` from the defaults. Building another way, run `idf.py set-target esp32c6` first.

`sdkconfig.defaults` sets 4 MB flash, the custom partition table, the USB-Serial/JTAG console, the
ZC/ZR Zigbee library, the native radio, Bluetooth off and the task watchdog.

## Partition table (`partitions.csv`)

| Name | Type | Size | Contents |
|---|---|---|---|
| nvs | data/nvs | 24 KB | Settings (profile, EPID filter, channel and TCLK overrides) |
| phy_init | data/phy | 4 KB | RF calibration |
| factory | app | 1.875 MB | Firmware (about 660 KB used) |
| zb_storage | data/fat | 16 KB | Zigbee NVRAM: network membership, keys, frame counters |
| zb_fct | data/fat | 1 KB | Zigbee production config (unused; the SDK requires it) |

`erase-flash` wipes everything, including network membership. Flashing only the application keeps
`zb_storage` and `nvs`, so the device stays joined.

## Tests without hardware

```bash
tests/run_all.sh        # host C unit tests for main/diag_logic.c + Python tool tests
```
