# WattPill 2.0.0

Battery level **and remaining time** for wireless mice, headsets and controllers — right on the
Windows taskbar, without vendor software.

## Renamed

WattPill is the new name of JuiceBar. This is a rename release: the functionality is the same as
JuiceBar 1.0.5.

## What changed

- The app, executable, installer, window classes, autostart value and data folders use the new name.
- Settings and history from JuiceBar 1.x are copied automatically on first launch to
  `%APPDATA%\WattPill` and `%LOCALAPPDATA%\WattPill`. The old JuiceBar autostart value is removed.
- New installer identity and install folder. If you have JuiceBar installed, install WattPill and
  then uninstall JuiceBar; the old settings and history are already migrated.

## What the app does

- Native Win32 + GDI+ taskbar pill with the real device percentage and an estimated remaining time.
- Click the pill for a compact panel with device cards; it closes 200 ms after the pointer leaves.
- Device drivers for SteelSeries Aerox/Prime, Razer Barracuda X, HyperX Cloud/Stinger, Sony PULSE 3D,
  Xbox controllers and Bluetooth devices whose battery Windows exposes.
- Placement above the clock (default) or on the taskbar left of the tray.
- English and Russian UI; the first run follows Windows (Russian Windows → Russian, everything else
  English). The choice is saved in `settings.ini`.
- No telemetry, no account, no WebView2/Electron; history stays local.

## Install

Download `WattPill-2.0.0-setup.exe` and run it. Per-user installer: no administrator rights, no UAC
prompt. By default it installs into `%LOCALAPPDATA%\Programs\WattPill` (the wizard lets you choose
another folder), adds a Start menu shortcut and registers an uninstaller.

The installer is not code-signed yet, so Windows SmartScreen may ask for confirmation:
**More info → Run anyway**.

## Requirements

Windows 10 1903+ or Windows 11, x64. No runtime to install.

## Notes

- The pill floats above the clock by default (*Placement → On the taskbar* puts it left of the tray)
  on a horizontal taskbar.
- While the Start menu is open, Windows keeps its own shell window above every third-party overlay;
  the pill becomes clickable again as soon as the menu closes.
- Settings: `%APPDATA%\WattPill\settings.ini`; history: `%LOCALAPPDATA%\WattPill\history.tsv`.
  Uninstalling keeps them; delete the folders to remove everything.
- Winget package `WattPill.WattPill` is being submitted separately; the GitHub installer above is
  the primary download.
