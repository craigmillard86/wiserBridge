# Building

## Versions (pinned deliberately)

| Component | Version | Why |
|---|---|---|
| ESP-IDF | **v5.5.6** | Latest v5.5 bug-fix release; officially recommended by esp-zigbee-sdk |
| esp-zigbee-lib | **1.6.8** (`~1.6.8`) | esp-zigbee-sdk **v1.x LTS**, ZBOSS-based, Zigbee 3.0 certified stack with years of field use (it is also what the Arduino Zigbee range-extender example uses) |
| esp-zboss-lib | **1.6.4** (`~1.6.4`) | Matching ZBOSS library |

esp-zigbee-sdk **v2.x** (released April 2026, Espressif's own new stack) is recommended by Espressif
for new designs. It was not chosen for the first version because its stack is new and
heating reliability matters more than features; v1.x is maintained (bug fixes). Porting to v2.x is
a contained change (APIs are renamed; see Espressif's migration guide) and is a sensible later step
once the Wiser interoperability questions are answered on v1.x.

Dependencies are fetched automatically by the IDF Component Manager from `main/idf_component.yml`.

## Option A — Docker (no local toolchain; what was used for verification)

Requires Docker Desktop with WSL integration enabled for your distro.

```bash
cd /mnt/c/Users/<you>/Documents/GIT/WiserBridge
tools/build.sh
```

This builds the router (`build/`), the sniffer (`sniffer/build/`) and merged single-file images in
`release/` with `SHA256SUMS.txt`. Equivalent manual commands:

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5.6 idf.py set-target esp32c6
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5.6 idf.py build
```

## Option B — Native ESP-IDF on Windows

1. Install **ESP-IDF v5.5.6** with the Espressif "ESP-IDF Tools Installer" for Windows
   (choose v5.5.6, target esp32c6).
2. Open the "ESP-IDF 5.5 PowerShell" (or CMD) shortcut, then:

```powershell
cd C:\Users\<you>\Documents\GIT\WiserBridge
idf.py set-target esp32c6
idf.py build
idf.py -p COM8 flash monitor        # replace COM8 with your port (see flashing.md)
```

Sniffer:

```powershell
cd sniffer
idf.py set-target esp32c6
idf.py build
```

## Configuration

`idf.py menuconfig` → **WiserBridge Router**:

* *Device identity* — manufacturer/model strings, endpoint, profile/device ID, node manufacturer
  code, Identify cluster. Defaults are generic (`DIY-ESP32C6` / `ZB-ROUTER-C6`, HA Range Extender
  0x0008). Most identity fields can also be changed at runtime with `profile set` (no rebuild).
* *Zigbee network* — max children (default 20), network size (table sizing, default 64), join
  channel mask, TCLK exchange requirement, TX power (18 dBm), join window, auto-join (off),
  isolation alert.
* *Diagnostics* — HEALTH line interval (default 300 s).
* *Board* — RF switch / antenna / LED / button GPIOs, reset hold time.

`sdkconfig.defaults` sets the 4 MB flash, the custom partition table, USB-Serial/JTAG console, the
ZC/ZR Zigbee library, native radio, Bluetooth off and the task watchdog.

## Partition table (`partitions.csv`)

| Name | Type | Size | Purpose |
|---|---|---|---|
| nvs | data/nvs | 24 KB | application settings (profile, EPID filter, channel/TCLK overrides) |
| phy_init | data/phy | 4 KB | RF calibration |
| factory | app | 1.875 MB | firmware (≈660 KB used) |
| zb_storage | data/fat | 16 KB | **Zigbee NVRAM**: network membership, keys, frame counters |
| zb_fct | data/fat | 1 KB | Zigbee production config (unused; required by the SDK) |

`erase-flash` wipes everything including network membership. Re-flashing the application alone
keeps `zb_storage` and `nvs`, so the device stays joined across firmware updates.

## Tests that need no hardware

```bash
tests/run_all.sh        # host C unit tests for main/diag_logic.c + Python tool tests
```
