# Flashing on Windows

## 1. Find the COM port

Plug the XIAO ESP32-C6 in with a **data** USB-C cable. It appears as *USB Serial Device (COMx)* with
VID **303A** / PID **1001** (native USB-Serial/JTAG — no driver needed on Windows 10/11).

PowerShell:

```powershell
Get-PnpDevice -PresentOnly | Where-Object InstanceId -match 'VID_303A' | Select FriendlyName, InstanceId
```

On the development PC it was **COM8** — yours may differ. Bluetooth "Standard Serial over Bluetooth
link" ports are not the board.

## 2. Install esptool (once)

```powershell
py -m pip install --upgrade esptool pyserial
py -m esptool version
```

## 3. Flash

**Recommended first time: erase everything**, so no stale Zigbee network state from earlier
firmware can make the board rejoin some network:

```powershell
py -m esptool --chip esp32c6 -p COM8 erase-flash
```

Then either the merged release image:

```powershell
py -m esptool --chip esp32c6 -p COM8 -b 460800 write-flash 0x0 release\wiserbridge-router-0.1.0-xiao-esp32c6.bin
```

or the individual build outputs:

```powershell
cd build
py -m esptool --chip esp32c6 -p COM8 -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"
```

or, with native ESP-IDF: `idf.py -p COM8 flash monitor`.

**Updating firmware later:** flash only the application, *without* `erase-flash`:

```powershell
py -m esptool --chip esp32c6 -p COM8 -b 460800 write-flash 0x10000 build\wiser_bridge_router.bin
```

Network membership (`zb_storage` at 0x1F0000) and your saved settings (`nvs`) are kept and the
repeater rejoins on boot. Note: the merged `release\*.bin` image is one contiguous file from 0x0, so
its padding **overwrites the `nvs` region** (profile, `epid`, `channels`, `tclk` settings revert to
defaults) — it does not touch `zb_storage`. Use it for first installs; use the app-only command for
updates.

If flashing cannot connect: hold **BOOT**, press **RESET** (or re-plug USB) while holding BOOT,
release BOOT, then flash again.

## 4. Open the console

```powershell
py tools\wb_monitor.py --port COM8 --duration 0
```

`wb_monitor.py` does not reset the board when it attaches (safe during soak tests). Add `--reset`
to reboot and capture the boot banner, `--cmd status` to send commands, `--log logs\x.log` to keep a
timestamped log. `idf.py -p COM8 monitor`, PuTTY or Windows Terminal + any serial tool also work;
the message *"Your terminal application does not support escape sequences"* is harmless.

Type `help`. A healthy first boot shows the banner (firmware, chip, IDF, zigbee-lib, role, endpoint,
identity, child limit, channels, EPID filter, TCLK mode, TX power, antenna) followed by:

```
I (...) WB_EVENT: START: factory-new, not commissioned
W (...) WB_ZB: Not commissioned. Put the Wiser hub into pairing mode, then type 'join' ...
```

## Switching to/from the sniffer firmware

```powershell
py -m esptool --chip esp32c6 -p COM8 write-flash 0x0 release\wiserbridge-sniffer-0.1.0-xiao-esp32c6.bin
```

The sniffer does not touch `zb_storage`, so flashing the router back afterwards (without
`erase-flash`) restores the router with its network membership intact. To also keep your `nvs`
settings, restore with the three-segment `build\flash_args` command rather than the merged image. While the sniffer is
flashed, the board is **not** routing — children it had will move to other parents.
