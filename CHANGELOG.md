# Changelog

All notable changes to JuiceBar are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.2] — 2026-11-23

### Added

- HyperX Cloud II Wireless, Cloud Alpha Wireless and Cloud Flight Wireless: battery level over the
  USB dongle (the headset's own percentage for Cloud II / Alpha, a voltage-based estimate for the
  Flight) and the charging flag.
- HyperX Cloud Stinger Core Wireless + 7.1 (`0951:170B`): feature-report connection and battery
  queries, charge code and voltage curves, with the charging flag.
- An experimental probe for other HyperX dongles (HP `0x03f0`, Kingston `0x0951`): the common
  vendor protocols are tried and a device is shown only when the dongle actually answers.

### Notes

- The Stinger Core dongle only answers while the headset is powered on; when it is off, JuiceBar
  keeps showing the last level until the device disappears.

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

- Settings → Placement in the right-click menu: *Above the clock* (default) or *On the taskbar*
  (the classic position left of the tray).
- The pill now floats just above the clock, right-aligned; it grows to the left as devices connect,
  so it never covers taskbar buttons, toolbars or the news widget. The panel opens above the pill.
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
