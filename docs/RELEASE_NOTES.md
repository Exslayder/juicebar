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
- **Experimental HyperX probe.** Any other HyperX dongle (HP `0x03f0` or Kingston `0x0951`) that
  exposes a vendor HID collection is queried with the common protocols; the device is shown only
  when it actually answers. This is how we test models that are not on the known list yet —
  including the Cloud Stinger Core Wireless.
- Everything from 1.0.1: borderless-fullscreen hiding, fast startup enumeration, single-instance
  fix, Material Symbols device glyphs, reproducible builds.

## Honest note about the Cloud Stinger Core Wireless

Its dongle appears to be an audio-only device (HyperX's own software does not recognize this
model). If it has no vendor HID interface, there is no battery value in Windows for any app to
show — not just JuiceBar. The probe above will tell us quickly: if the headset shows up with a
percentage, it works; if not, the hardware does not report it.

## Requirements

Windows 10 1903+ or Windows 11, x64. No runtime to install.

## Notes

- The pill lives next to the notification area on a horizontal taskbar.
- While the Start menu is open, Windows keeps its own shell window above every third-party
  overlay; the pill becomes clickable again as soon as the menu closes.
- Settings: `%APPDATA%\JuiceBar\settings.ini`; history: `%LOCALAPPDATA%\JuiceBar\history.tsv`.
  Uninstalling keeps them; delete the folders to remove everything.
