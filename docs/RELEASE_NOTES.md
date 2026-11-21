# JuiceBar 1.0.0

Battery level **and remaining time** for wireless mice, headsets and controllers — right on the
Windows taskbar, with no vendor software.

## Install

Download `JuiceBar-1.0.0-setup.exe` and run it. The installer is per-user: no administrator
rights, no UAC prompt. It installs to `%LOCALAPPDATA%\Programs\JuiceBar`, adds a Start menu
shortcut, and registers an uninstaller.

Prefer a portable copy? `JuiceBar-1.0.0-portable.zip` runs from any folder and keeps settings and
history next to the exe.

## What's new

- First public release.
- Taskbar pill with the real percentage of each connected device; click it for a compact panel
  with the name, remaining time, level and battery glyph.
- Native Win32 + GDI+: ~1.8 MB exe, no WebView2/Electron/Chromium, ~23 MB RAM, 0 ms idle CPU.
- Drivers for SteelSeries Aerox/Prime wireless mice, Razer Barracuda X (2022), any Bluetooth
  device Windows exposes a battery level for, and Xbox controllers via XInput.
- Remaining-time forecast from measured discharge, with a clearly labelled nominal estimate while
  the data is thin.
- Start with Windows (on by default, toggle in the right-click menu), Russian/English UI,
  automatic light/dark theme, 100–200 % DPI.

## Requirements

Windows 10 1903+ or Windows 11, x64. No runtime to install.

## Notes

- The taskbar pill lives next to the notification area on a horizontal taskbar.
- While the Start menu is open, Windows keeps its own shell window above every third-party
  overlay; the pill becomes clickable again as soon as the menu closes.
- Settings: `%APPDATA%\JuiceBar\settings.ini`; history: `%LOCALAPPDATA%\JuiceBar\history.tsv`.
  Uninstalling keeps them; delete the folders to remove everything.
