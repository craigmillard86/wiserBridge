# Flashing on Windows

## From the browser

Open https://craigmillard86.github.io/wiserBridge/ in Chrome or Edge. Pick the board, click
Install and choose its port. Erase on a new board or when switching firmware. Don't erase when
updating a joined router: it keeps its network membership and settings. The Console page on the
same site replaces `wb_monitor.py`.

The site is built by `.github/workflows/pages.yml` on every push to `master`. It flashes the
bootloader, partition table and app as separate parts at 0x0, 0x8000 and 0x10000, so NVS is not
overwritten.

The rest of this page covers flashing with esptool.

## 1. Find the COM port

Connect the XIAO ESP32-C6 with a USB-C data cable. It appears as "USB Serial Device (COMx)", VID
303A / PID 1001. No driver is needed on Windows 10 or 11.

```powershell
Get-PnpDevice -PresentOnly | Where-Object InstanceId -match 'VID_303A' | Select FriendlyName, InstanceId
```

The examples use COM8; use your own port. "Standard Serial over Bluetooth link" ports are not the
board.

## 2. Install esptool (once)

```powershell
py -m pip install --upgrade esptool pyserial
py -m esptool version
```

## 3. Flash

### First install

1. Erase the flash, so no Zigbee state from earlier firmware can make the board rejoin another
   network:

   ```powershell
   py -m esptool --chip esp32c6 -p COM8 erase-flash
   ```

2. Write the merged release image:

   ```powershell
   py -m esptool --chip esp32c6 -p COM8 -b 460800 write-flash 0x0 release\wiserbridge-router-0.1.0-xiao-esp32c6.bin
   ```

   Or write your own build outputs:

   ```powershell
   cd build
   py -m esptool --chip esp32c6 -p COM8 -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"
   ```

   Or, with native ESP-IDF: `idf.py -p COM8 flash monitor`.

### Updates

Flash only the application, without `erase-flash`:

```powershell
py -m esptool --chip esp32c6 -p COM8 -b 460800 write-flash 0x10000 build\wiser_bridge_router.bin
```

This keeps network membership (`zb_storage` at 0x1F0000) and saved settings (`nvs`). The device
rejoins on boot.

Do not use the merged `release\*.bin` image for updates. It is written from 0x0 and overwrites
`nvs`, which resets the profile, `epid`, `channels` and `tclk` settings. It does not touch
`zb_storage`.

### If esptool cannot connect

Hold BOOT, press RESET (or replug USB), release BOOT, then flash again.

## 4. Open the console

```powershell
py tools\wb_monitor.py --port COM8 --duration 0
```

Opening the USB-Serial/JTAG port reboots the board. It keeps its network membership.

`wb_monitor.py` options: `--reset` captures the boot banner, `--cmd status` sends a command,
`--log logs\x.log` saves a timestamped log. `idf.py -p COM8 monitor` or PuTTY also work. The
message "Your terminal application does not support escape sequences" is harmless.

Type `help` for the command list. A first boot prints a banner (firmware, versions, role, identity,
child limit, channels, EPID filter, TCLK mode, TX power, antenna), then:

```
I (...) WB_EVENT: START: factory-new, not commissioned
W (...) WB_ZB: Not commissioned. Put the Wiser hub into pairing mode, then type 'join' ...
```

The identity should be `HEIMAN` / `SmartPlug-EF-3.0`. If it is `DIY-ESP32C6` / `ZB-ROUTER-C6`, run
`profile defaults`, `profile save` and `reboot` before joining.

## Sniffer firmware

```powershell
py -m esptool --chip esp32c6 -p COM8 write-flash 0x0 release\wiserbridge-sniffer-0.1.0-xiao-esp32c6.bin
```

The sniffer does not touch `zb_storage`. Flash the router back without `erase-flash` and it keeps
its network membership. To also keep `nvs` settings, restore with the `build\flash_args` command,
not the merged image. While the sniffer is on the board it does not route, and its children move to
other parents.
