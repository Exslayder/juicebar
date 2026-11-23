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
- **Experimental HyperX probe.** Any other HyperX dongle (HP `0x03f0` or Kingston `0x0951`) that
  exposes a vendor HID collection is queried with the common protocols; the device is shown only
  when it actually answers. This is how models that are not on the known list get a chance to work
  without a new build.
- Everything from 1.0.1: borderless-fullscreen hiding, fast startup enumeration, single-instance
  fix, Material Symbols device glyphs, reproducible builds.

## Note about the Cloud Stinger Core Wireless

The dongle only answers while the headset is switched on. With the headset off, no level is
available; JuiceBar keeps the last known value dimmed for five minutes and then hides the device.

## Requirements

Windows 10 1903+ or Windows 11, x64. No runtime to install.

## Notes

- The pill lives next to the notification area on a horizontal taskbar.
- While the Start menu is open, Windows keeps its own shell window above every third-party
  overlay; the pill becomes clickable again as soon as the menu closes.
- Settings: `%APPDATA%\JuiceBar\settings.ini`; history: `%LOCALAPPDATA%\JuiceBar\history.tsv`.
  Uninstalling keeps them; delete the folders to remove everything.
