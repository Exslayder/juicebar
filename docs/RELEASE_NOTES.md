# WattPill 2.1.2

Battery level **and remaining time** for wireless mice, headsets and controllers — right on the
Windows taskbar, without vendor software.

## Fixed in 2.1.2

- The pill no longer jumps above the clock after repeated **Win + left-click** cycles over a
  fullscreen game. When the taskbar is briefly not enumerable, the pill now keeps its last taskbar
  position instead of using the "monitor without a taskbar" fallback.
- Included from 2.1.1: the pill stays hidden through Alt+Tab / Task View shell overlays, while the
  Win/Start menu correctly brings it back; device updates cannot force it visible during fullscreen.

![WattPill device panel and settings](https://raw.githubusercontent.com/Exslayder/wattpill/main/docs/screenshot.png)

## Highlights

- **New taskbar pill.** Rebuilt to sit naturally on the Windows 11 taskbar: inset height, Fluent
  glyphs, a 16×9.5 battery capsule, green charging, red low-battery highlighting and a small hover
  hint (`Shift` moves along the bar, `Ctrl` moves freely).
- **Settings panel instead of a menu.** Right-click the pill for the WattPill wordmark, one card of
  toggles and segmented options, and Refresh/Exit buttons. Everything applies immediately; there is
  no Save button, and the panel closes when the pointer leaves.
- **Per-device eye.** Hide a device from the pill; its card stays in the panel, desaturated, with a
  slashed eye to bring it back.
- **Any monitor.** Move the widget and its panel to the primary or an additional display, using that
  display's own taskbar.
- **Crisper digits.** The pill's percentages and glyphs are rendered with DirectWrite — the same
  grayscale rasterizer WPF uses — while the app stays a ~1.9 MB native Win32 exe.
- **More honest remaining time.** The estimate uses uninterrupted discharge runs from up to 30 days,
  prefers the last 12 hours, falls back to the 7-day median, and rounds to minutes, hours or days.
  When there is not enough data it says so instead of inventing a number.

## Fixes

- Fullscreen hiding now follows the foreground window on the widget's own monitor, so a maximized
  document no longer hides the pill and a fullscreen app on another display no longer affects it.
- The pill is repainted and re-anchored when fullscreen ends.
- Devices that stop answering are removed instead of being shown as disconnected.

## Install

Download `WattPill-2.1.2-setup.exe` and run it. Per-user installer: no administrator rights, no UAC
prompt. By default it installs into `%LOCALAPPDATA%\Programs\WattPill` (the wizard lets you choose
another folder), adds a Start menu shortcut and registers an uninstaller.

The installer is not code-signed yet, so Windows SmartScreen may ask for confirmation:
**More info → Run anyway**.

## Requirements

Windows 10 1903+ or Windows 11, x64. No runtime to install.

## Notes

- Left-click the pill for the device panel; right-click for settings. The pill follows a horizontal
  taskbar.
- While the Start menu is open, Windows keeps its own shell window above every third-party overlay;
  the pill becomes clickable again as soon as the menu closes.
- Settings: `%APPDATA%\WattPill\settings.ini`; history: `%LOCALAPPDATA%\WattPill\history.tsv`.
  Uninstalling keeps them; delete the folders to remove everything.
- The Winget package `WattPill.WattPill` is being updated to 2.1.2; the GitHub installer above is
  the primary download.
