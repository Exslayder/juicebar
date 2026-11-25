# WattPill

[![release](https://img.shields.io/github/v/release/Exslayder/wattpill.svg?label=release)](https://github.com/Exslayder/wattpill/releases/latest)
[![build](https://github.com/Exslayder/wattpill/actions/workflows/release.yml/badge.svg)](https://github.com/Exslayder/wattpill/actions/workflows/release.yml)
[![UI](docs/ui-badge.svg)](#languages)
[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-0078D4)](#requirements)
[![telemetry](https://img.shields.io/badge/telemetry-none-brightgreen)](#data--privacy)

Battery level **and remaining time** for wireless mice, headsets and controllers — right on the
Windows taskbar, without vendor software.

![WattPill panel and pill in English](docs/screenshot.png)

A small pill floats above the clock (or sits on the taskbar, if you prefer) and shows the real level
reported by each device. Click it and a compact panel opens with the device name, the estimated time
left, the percentage and a battery glyph. It closes 200 ms after the pointer leaves it (or on a
second click on the pill). Right-click opens the settings panel.

> **Renamed from JuiceBar.** WattPill is the new name. Settings and history from JuiceBar 1.x are
> migrated on first launch; if you have JuiceBar installed, uninstall it after installing WattPill.

## Why another battery monitor

Most tools only repeat a percentage. WattPill answers the question you actually have: *"how long
until it dies?"* — and it is honest about it: a forecast is shown as a forecast, never as a
measurement, and it is labelled while the data is still thin.

- **Real device percentages.** The Razer Barracuda X reports its own 0–100 % over the 2.4 GHz
  receiver (customer data `0x21`), not a voltage guess. SteelSeries mice report their level in
  fixed 5 % steps, exactly like the mouse firmware does.
- **Remaining time at a glance.** A local estimate over uninterrupted discharge runs from up to
  30 days gives `≈ 3 ч` or `≈ 3 дня`; when there is not enough data it says so instead of inventing a
  number. Nothing is uploaded and no account is needed.
- **All native, no Chromium.** ~1.9 MB exe: the pill, the panel, device polling and history are
  plain Win32; the panel uses GDI+, the pill's digits use DirectWrite (the same rasterizer as WPF).
  No WebView2, no Electron, no background Chromium processes. Idle CPU is 0 ms/s and the working
  set is under 25 MB.
- **Sits on the taskbar, not over it.** The pill is inset into the taskbar, centered vertically, and
  uses the same height and hover weight as the Explorer tray buttons. *Settings → Placement → On the
  taskbar* docks it left of the tray; *Above the clock* floats it over the clock. Shift-drag slides
  it along the bar; Ctrl-drag (or *Move widget*) moves it freely anywhere on the monitor; *Reset
  position* puts it back.
- **A panel that behaves like a shell flyout.** It opens centered on the pill with a short slide and
  fade, keeps WattPill's own look (Inter, grouped cards, violet forecast accent) and contains no
  chrome. Every card has a small eye button: hide a device from the pill and its row stays in the
  panel, desaturated, so it can be brought back.
- **A settings panel, not a menu.** Right-click the pill: the WattPill wordmark, a card of toggle
  switches and segmented options, and Refresh/Exit buttons. Everything applies immediately; the
  panel closes when the pointer leaves.
- **Color that means something.** Charging is green on the pill and in the panel; at 20 % or below
  the same elements (glyph, capsule/battery mark, percentage and status) turn red.
- **A pill that matches the taskbar.** Full (glyph, mini battery capsule, percentage), Compact
  (glyph and percentage) or Minimal (percentage only); optional Fluent background plate. While
  charging, the bolt and the percentage turn green.
- **Any monitor.** *Settings → Monitor* moves the pill and panel to the primary or an additional
  display, using that display's own taskbar.
- **Starts with Windows.** *Settings → Start with Windows* toggles a single `HKCU\…\Run` value, no
  service and no admin rights.
- **Follows Windows.** The panel follows the app theme, the pill follows the taskbar theme, live;
  100–200 % DPI aware.
- **English and Russian UI.** A Russian Windows starts in Russian, everything else in English;
  *Settings → Language* switches and saves the choice. Menus, panel labels and duration strings are
  translated.

## Install

Download `WattPill-2.1.1-setup.exe` from the latest release and run it. The installer is per-user —
no administrator rights, no UAC prompt. By default it installs into
`%LOCALAPPDATA%\Programs\WattPill` and adds a Start menu shortcut; the wizard also lets you pick
another folder (for example `D:\Apps\WattPill`).

Release binaries are built by GitHub Actions from the tagged commit; the workflow runs the unit
tests, then produces the installer.

To remove the app, use *Settings → Apps → Installed apps → WattPill* (or *Apps & features* on
Windows 10) or the uninstaller in the install folder. Your settings and history are kept; delete
`%APPDATA%\WattPill` and `%LOCALAPPDATA%\WattPill` to remove them too.

## Placement

The pill floats above the clock by default. Right-click → *Settings* → *Placement* → *On the
taskbar* puts it left of the tray; *Above the clock* brings it back.

Shift-drag the pill to move it along the taskbar, hold Ctrl (or enable *Move widget*) to drag it
freely anywhere on the monitor. *Reset position* returns it to the default spot; the position is
saved in `settings.ini` (`pill_free`, `pill_free_x/y`, or `pill_manual` + `pill_offset`).

## Supported devices

| Device family | Transport | What is read |
|---|---|---|
| SteelSeries Aerox 3 / 3 Gen 2 / 5 / 9 Wireless, Prime Wireless, CS2 and Diablo IV editions | 2.4 GHz receiver or USB cable | firmware battery step (0–100 % in 5 % steps; Gen 2 in 1 % steps) + charging flag |
| Razer Barracuda X (2022), PIDs `1532:0550` / `1532:0552` | 2.4 GHz dongle | headset's own percentage (`0x21`) + cable state (`0x2A`) |
| HyperX Cloud II Wireless, Cloud Alpha Wireless, Cloud Flight Wireless | 2.4 GHz USB dongle | the headset's own percentage (Cloud II / Alpha) or a voltage-based estimate (Flight), plus the charging flag |
| HyperX Cloud Stinger Core Wireless + 7.1 (`0951:170B`) | 2.4 GHz USB dongle | feature-report charge code + voltage estimate (charging / discharging curves) |
| Other HyperX dongles (experimental) | 2.4 GHz USB dongle | tries the same vendor protocols (including the feature-report one); a device is shown only if the dongle answers |
| Sony PULSE 3D (`054C:0D5E`) | 2.4 GHz USB dongle | pushed status reports: level in 10 % steps, charging state and connection flag (the dongle answers no queries; shown as `?` until the first report) |
| Xbox controllers (XInput) | wireless | coarse level (empty / low / medium / full → 5 / 20 / 60 / 100 %) |
| Bluetooth devices with a Windows battery level | Bluetooth LE / classic | the level Windows itself exposes (headsets, earbuds, mice, keyboards) |

Everything else is ignored: WattPill only queries the devices it recognises and never changes their
settings. Speakers that do not publish a battery level (most JBL, for example) cannot be shown —
Windows has no number to read.

## Accuracy

- A percentage is only shown when the device actually reports one (`0…100`). Out-of-range values
  are rejected instead of clamped silently.
- A device that stops answering is removed from the pill and the panel as soon as it disappears from
  the poll. A receiver that stays plugged in keeps its headset on screen as "off" (dimmed) with the
  last level only while the receiver itself is still present.
- SteelSeries reports steps, so 45 % means "45 %"; Razer reports its own number; Xbox levels are
  coarse by nature and shown as such.
- Verified twice against the reference protocol reader (BatteryHub) with both tools running at the
  same moment: `75 % / 49 %` and `45 % / 49 %` for the Aerox 3 Wireless and Barracuda X — identical.
- Forecasts are only shown when the device has actually lost charge in an uninterrupted discharge
  run; otherwise the panel says "Not enough data yet" instead of inventing a number.

## Remaining time

The forecast is built from the discharge history, split into uninterrupted runs (a charging session,
a PC reboot or a gap longer than 30 minutes starts a new run). It uses up to 30 days of history:

- a fresh run from the last 12 hours is preferred when it lost at least 2 %;
- otherwise the median rate of the last 7 days is used;
- the result is rounded (minutes up to an hour, then hours, then whole days) and always shown with
  `≈`, because usage changes the real runtime.

| State | Panel text |
|---|---|
| no measurable drop yet | `2.4 GHz · Мало данных для прогноза` |
| forecast, hours | `2.4 GHz · ≈ 12 ч` |
| forecast, days | `2.4 GHz · ≈ 3 дня` |
| charging | `2.4 GHz · Заряжается · до полного 45 мин` when the charge rate is known |

## Polling

- Devices are polled once every **15 seconds** by a background thread. The Sony PULSE 3D is the
  exception: its dongle only pushes reports, so a second thread blocks on them and wakes the poller
  when something changes.
- An extra read happens immediately on start, on a device connect/disconnect (`WM_DEVICECHANGE`),
  after resume from sleep, and from *Refresh* in the menu.
- History keeps at most one sample per device per minute and is written to disk every 5 minutes
  and on exit.

## Settings

Left-click the pill opens the device panel; **right-click opens Settings** directly.

The settings panel uses the same card language as the device panel: the **WattPill** wordmark
top-left, a small *Настройки* caption top-right, one rounded card with a row per option, and two
buttons at the bottom. Everything applies immediately — there is no Save button — and the panel
closes when the pointer leaves it.

- **Start with Windows** — toggle.
- **Move widget** — toggle; when on, a plain drag moves the pill freely.
- **Pill background** — toggle the Fluent plate.
- **Hide in fullscreen** — toggle.
- **Placement** — *Above clock* / *On taskbar*.
- **Pill style** — *Full* / *Compact* / *Minimal*.
- **Monitor** — *Основной* / *Дополнительный* (only shown with more than one display).
- **Language** — *Auto* / *Рус* / *Eng*.
- **Reset position** — returns the pill to its default taskbar spot.
- **Refresh** — poll the devices right now.
- **Exit** — close WattPill.

Hovering the pill shows a small hint: *Shift — move along the taskbar* and *Ctrl — move freely*.
With *Move widget* enabled, a plain drag moves freely as well.

## Languages

The first run follows the Windows UI language: a Russian Windows uses Russian, everything else
uses English. *Settings → Language* switches between *Auto*, *Рус* and *Eng*; the choice is saved in
`settings.ini` and applies immediately, including the duration strings in the panel.

## Command line

```
WattPill.exe --once          print detected devices and exit
WattPill.exe --json          same as JSON
WattPill.exe --dump          list HID collections (for adding new devices)
WattPill.exe --fonts         report which UI fonts were resolved
WattPill.exe --fullscreen    report the fullscreen detection and exit
WattPill.exe --panel         start with the panel open
WattPill.exe --no-pill       hide the taskbar pill for this run
WattPill.exe --data <dir>    keep settings and history in <dir> (portable)
WattPill.exe --quit          close the running instance
```

## Requirements

- Windows 10 1903+ or Windows 11, x64.
- No administrator rights, no installer services, no runtime to install.
- About 25 MB of RAM and no measurable idle CPU.

## Build from source

The repository builds with MinGW-w64 (UCRT). A pinned portable toolchain can be fetched into the
project (no system installation):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\toolchain.ps1
powershell -ExecutionPolicy Bypass -File build.ps1 -Version 2.1.1
powershell -ExecutionPolicy Bypass -File test.ps1
```

`build.ps1` compiles the native sources in `src\`, embeds the icons and the three Inter faces, and
writes a single self-contained `build\WattPill.exe`.

The build is reproducible: the linker does not insert a PE timestamp, so two builds of the same
commit with the same toolchain are byte-identical (SHA-256), and anyone can verify a release
binary by rebuilding it. The toolchain script also verifies the SHA-256 of the downloaded
MinGW archive before extracting it.

To build the installer you need [Inno Setup 6+](https://jrsoftware.org/isinfo.php):

```powershell
powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1 -Version 2.1.1
```

The result is `dist\WattPill-<version>-setup.exe`.

## Data & privacy

- Settings: `%APPDATA%\WattPill\settings.ini`
- History: `%LOCALAPPDATA%\WattPill\history.tsv` (30 days, sampled once a minute)
- Autostart: `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\WattPill`
- Nothing is sent anywhere: there is no HTTP client in the binary.
- `--data <dir>` or a `portable.flag` file next to the exe keeps everything in one folder.

## Limitations

- The pill follows a horizontal taskbar; vertical taskbars are not supported. It floats above the
  clock by default; *Settings → Placement → On the taskbar* puts it left of the tray.
- Windows has no supported API to pin a layered popup above its own topmost taskbar. After a
  screenshot or a taskbar focus change the shell can raise itself above the pill; a read-only
  z-order check once per second re-asserts the pill only when that actually happened (zero work
  and zero z-order calls while everything is stable).
- Bluetooth support depends on Windows exposing a battery level; many speakers do not.
- Only the device families above are queried. More drivers can be added one at a time.

## Design

The visual system, icon sources and the taskbar z-order notes are documented in
[docs/DESIGN.md](docs/DESIGN.md).

## Credits

See [CREDITS.md](CREDITS.md). Icons: Material Symbols Rounded (Apache-2.0) and Fluent System Icons
(MIT). Text: Inter (OFL). The application icon is the Fluent `flash` glyph on a generated violet
squircle.

## License

MIT — see [LICENSE](LICENSE).
