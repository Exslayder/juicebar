# JuiceBar 1.0.2

Battery level **and remaining time** for wireless mice, headsets and controllers — right on the
Windows taskbar, with no vendor software.

## Install

Download `JuiceBar-1.0.2-setup.exe` and run it. Per-user installer: no administrator rights, no UAC
prompt. By default it installs into `%LOCALAPPDATA%\Programs\JuiceBar` (the wizard lets you choose
another folder), adds a Start menu shortcut and registers an uninstaller.

## New in this build

- **HyperX support.** HyperX Cloud II Wireless, Cloud Alpha Wireless and Cloud Flight Wireless are
  read over their USB dongle: the headset's own percentage for Cloud II / Alpha, a voltage-based
  estimate for the Flight, plus the charging flag.
- **Cloud Stinger Core Wireless + 7.1** (`0951:170B`): the dongle's feature-report protocol is
  implemented too (connection check, charge code, voltage curves), so this model now reports its
  level and charging state.
- **Sony PULSE 3D** (`054C:0D5E`): the 2.4 GHz dongle pushes status reports with the level (10 %
  steps), the charging state and the connection flag. A background reader keeps the collection open
  and blocks in `ReadFile`, so a report is never missed and the pill refreshes immediately; the
  device is visible with `?` until the first report.
- **Above the clock, never over the taskbar.** The pill floats just above the clock. Its right
  edge stays in place and it grows left as devices connect, so taskbar buttons, toolbars and the
  news widget stay uncovered; the panel opens above it.
- **Windows 10 fix.** The pill no longer disappears for ordinary maximized documents: `QUNS_BUSY`
  and maximized windows are ignored by the fullscreen check now.
- **Placement setting.** Right-click → Placement lets you choose *Above the clock* (default) or
  *On the taskbar*.
- **Experimental HyperX probe.** Any other HyperX dongle (HP `0x03f0` or Kingston `0x0951`) that
  exposes a vendor HID collection is queried with the common protocols; the device is shown only
  when it actually answers. This is how models that are not on the known list get a chance to work
  without a new build.
- Everything from 1.0.1: borderless-fullscreen hiding, fast startup enumeration, single-instance
  fix, Material Symbols device glyphs, reproducible builds.

## Note about the Cloud Stinger Core Wireless

The dongle only answers while the headset is switched on. With the headset off, no level is
available; JuiceBar keeps the last known value dimmed for five minutes and then hides the device.

## Note about the PULSE 3D

The dongle only reports on change: power on/off, a button press, the charge cable, or a 10 % battery
step (roughly once an hour), and only to a reader that is already open. JuiceBar keeps the collection
open permanently, so it catches every change and updates the pill immediately. Right after a start
there may be no status yet: the device is shown with `?` until you press a volume button on the
headset. While charging the dongle withholds the level, so JuiceBar shows the last known percentage
with the green charging mark.

## Requirements

Windows 10 1903+ or Windows 11, x64. No runtime to install.

## Notes

- The pill lives next to the notification area on a horizontal taskbar.
- While the Start menu is open, Windows keeps its own shell window above every third-party
  overlay; the pill becomes clickable again as soon as the menu closes.
- Settings: `%APPDATA%\JuiceBar\settings.ini`; history: `%LOCALAPPDATA%\JuiceBar\history.tsv`.
  Uninstalling keeps them; delete the folders to remove everything.
