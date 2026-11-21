# Changelog

All notable changes to JuiceBar are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
