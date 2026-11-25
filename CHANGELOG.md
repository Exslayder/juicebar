# Changelog

All notable changes to WattPill are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- PowerVision-style taskbar anchoring: the pill is inset and centered in the taskbar, follows its
  height, and hides while the taskbar auto-hides. A 300 ms guard plus a foreground WinEvent hook
  keeps it above `Shell_TrayWnd` without `SetParent`; Explorer restarts and display changes
  re-anchor it.
- Manual pill position: Shift-drag moves it along the taskbar; hold Ctrl (or *Placement → Move
  widget*) to drag it freely anywhere on the monitor; *Placement → Reset position* returns to the
  default.
- Hovering the pill shows a two-line hint: *Shift — двигать по панели* and *Ctrl — свободно по
  экрану*.
- Pill styles: detailed (glyph, battery capsule, percentage), compact (glyph and percentage) and
  percent only, plus an optional Fluent background plate.
- While charging, the pill bolt and percentage are green.
- *Hide in fullscreen* option.
- *Monitor* selector: *Основной* / *Дополнительный* (with an index when several), moving the pill
  and panel to that display.
- Settings open from the pill's right-click menu; the panel is pure content, without a wordmark,
  device count or gear.
- A settings panel in the device-panel style: toggle switches and segmented buttons for every
  option, applied immediately (no Save), closing when the pointer leaves. The pill's right-click
  menu is now just *Open panel*, *Refresh*, *Settings* and *Exit*.
- Dark/light native menu following the Windows app theme.
- Panel slide-and-fade animation.
- DirectWrite rendering for the pill text (the same grayscale rasterizer as WPF), with the GDI+ path
  as a fallback.

### Changed

- Redesigned the pill to match PowerVision's taskbar widget metric for metric: 40 DIP height,
  radius 5, inset taskbar placement, Fluent glyphs, 16×9.5 battery capsule with nub and charging
  bolt, 12 px semibold percentages and the Windows 11 taskbar palette.
- The panel keeps WattPill's own visual language (Inter, neutral cards, violet accent, Fluent
  battery marks) with no chrome.
- Devices that stop answering are removed instead of being shown as disconnected.
- Forecasts now use the observed discharge runs from up to 30 days (split by charging sessions and
  gaps), prefer the last 12 hours, fall back to the median of the last 7 days, round the result to
  minutes/hours/days, and show `Мало данных для прогноза` instead of a made-up "steady" state.
- Polling defaults to 15 seconds (PowerVision polls every 10 seconds) for fresher percentages.

### Fixed

- Fullscreen hiding now uses PowerVision's monitor-scoped rule: the foreground window must cover the
  widget's own monitor, be visible and uncloaked, and not be a shell window. This removes the
  QUNS/primary-taskbar heuristics that could hide the pill for a maximized window, keep it visible
  during a fullscreen app on the wrong monitor, or leave it hidden after exit. The pill repaints and
  re-anchors when it comes back.

## [2.0.0] — 2026-11-24

### Changed

- Renamed from JuiceBar to WattPill: executable, installer, window classes, autostart value and
  data folders use the new name.
- Settings and history from JuiceBar 1.x are copied to `%APPDATA%\WattPill` and
  `%LOCALAPPDATA%\WattPill` on first launch; the old JuiceBar autostart value is removed.
- New installer AppId and install folder. The old JuiceBar app can be uninstalled after WattPill is
  installed.

### Notes

- Functionality is unchanged from JuiceBar 1.0.5: the same device drivers, forecasts, placement
  options and English/Russian UI.

## [1.0.5] — 2026-11-24

### Added

- Language selection in the right-click menu: *Auto*, *Русский* or *English*. The choice is saved
  and applied immediately, including the duration strings; *Auto* uses a Russian Windows for Russian
  and everything else for English.

## [1.0.4] — 2026-11-23

### Fixed

- Windowed (fullscreen) games: a maximized borderless window (WoW's "Windowed (Fullscreen)" is
  one) now hides the pill. The check compares the window's client area with the monitor instead of
  rejecting every `IsZoomed` window, so an ordinary maximized document with a title bar still
  keeps the pill visible.

## [1.0.3] — 2026-11-23

### Fixed

- The calibration countdown no longer stays at "15 min": it uses the observed sample span and
  decreases with every poll. A device with a single sample shows "Calibrating" until a second
  sample arrives.

## [1.0.2] — 2026-11-23

### Added

- HyperX Cloud II Wireless, Cloud Alpha Wireless and Cloud Flight Wireless: battery level over the
  USB dongle (the headset's own percentage for Cloud II / Alpha, a voltage-based estimate for the
  Flight) and the charging flag.
- HyperX Cloud Stinger Core Wireless + 7.1 (`0951:170B`): feature-report connection and battery
  queries, charge code and voltage curves, with the charging flag.
- An experimental probe for other HyperX dongles (HP `0x03f0`, Kingston `0x0951`): the common
  vendor protocols are tried and a device is shown only when the dongle actually answers.
- Sony PULSE 3D (`054C:0D5E`): the dongle pushes status reports with the battery level (10 %
  steps) and the connection state. A background reader keeps its collection open and blocks in
  `ReadFile`, so every change is caught and the pill refreshes immediately; the device shows as
  "?" until the first report arrives.

### Fixed

- `--once` and `--help` now report the real build version instead of `0.1.0`; the numeric Windows
  resource version is generated from the same value instead of being hardcoded.
- Windows 10: the pill no longer hides for ordinary maximized documents. Windows 10 reports
  `QUNS_BUSY` for them, and with an auto-hidden taskbar a maximized window covers the whole
  monitor; both fooled the fullscreen check. It now ignores `QUNS_BUSY` and maximized windows and
  relies on the geometric check plus the two unambiguous D3D/presentation states.

### Changed

- Placement submenu in the right-click menu: *Above the clock* (default) or *On the taskbar*
  (the classic position left of the tray).
- The pill now floats just above the clock, right-aligned; it grows to the left as devices connect,
  so it never covers taskbar buttons, toolbars or the news widget. The panel opens above the pill.

### Notes

- The Stinger Core dongle only answers while the headset is powered on; when it is off, JuiceBar
  keeps showing the last level while the dongle stays plugged in.
- The PULSE 3D dongle only reports on change (power, a button, the cable, a 10 % step) and only to
  a reader that is already open. JuiceBar keeps the collection open permanently, so any change is
  caught and the pill updates right away. Right after a start, press a volume button on the headset
  to get the first status; until then the device is shown with "?".

## [1.0.1] — 2026-11-22

### Fixed

- The pill and panel now hide for borderless fullscreen games (any foreground window covering the
  monitor of the taskbar), not only for exclusive fullscreen.
- Startup could show "no devices" for up to 80 seconds: HID enumeration opened every device on the
  system and one sleeping Bluetooth interface blocked it. Only known vendor IDs are opened now
  (80 s → ~16 ms), and an empty poll is retried after 5 s.
- A stale `ERROR_ALREADY_EXISTS` could make the single-instance check exit the app silently; the
  last error is cleared before `CreateMutex`.

### Changed

- Device glyphs are Material Symbols Rounded (Apache-2.0) in the pill and the panel; battery marks
  stay Fluent. Charging is green, low battery red, sleeping devices dimmed.
- README badges and repository topics; the installer's folder choice is documented.
- Reproducible builds: no PE timestamp, and the pinned MinGW archive is verified by SHA-256.

## [1.0.0] — 2026-11-21

First public release.

### Added

- Taskbar pill with the real battery percentage of each connected wireless device, next to the
  notification area.
- Click-to-open panel with device name, remaining time, percentage and a battery glyph. It closes
  200 ms after the pointer leaves it, or on a second click on the pill.
- Forecast of the remaining time: a least-squares fit over up to six hours of discharge samples,
  with a nominal fallback per device kind while the data is thin.
- SteelSeries driver (Aerox 3 / 5 / 9 Wireless, Prime Wireless, CS2 and Diablo IV editions) over
  the 2.4 GHz receiver or USB, including the charging flag.
- Razer Barracuda X (2022) driver: the headset's own percentage and cable state over the dongle.
- Generic Bluetooth driver: any device whose battery level Windows publishes.
- Xbox controller support through XInput (coarse empty / low / medium / full levels).
- History file with at most one sample per device per minute, saved every five minutes and on exit.
- Start with Windows, enabled by default and toggleable from the right-click menu
  (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`).
- Right-click menu: Refresh, Start with Windows, Exit.
- Interface in Russian and English, following the system language.
- Light and dark themes taken from the taskbar, updated live.
- 100–200 % DPI support, per-pixel transparency, and an idle cost of zero measurable CPU.

### Notes

- Windows 10 1903+ / Windows 11, x64.
- No administrator rights are required; the installer is per-user.
