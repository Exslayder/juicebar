# Design system

WattPill is a native Win32 application. Everything you see is painted by hand on a
premultiplied 32-bit surface and pushed to the screen with `UpdateLayeredWindow`, so both the pill
and the panel get real per-pixel transparency, rounded edges and a soft shadow without a browser
engine.

## Surfaces

### Pill

The taskbar pill follows PowerVision's taskbar widget metric for metric: a 40 DIP window with a
38 DIP, 5 px-radius root, 8 DIP side padding and a 5 DIP margin around every device. A device is a
13.5 DIP Fluent glyph, a 16×9.5 mini battery capsule (1.1 DIP frame, 1 DIP inner padding, a 1.6×4.5
nub), an optional 9.5 DIP charging bolt and a 12 DIP semibold percentage. The capsule uses the
Windows 11 taskbar palette: green, amber up to 45 %, red up to 20 %, light blue while charging.

Three styles are available from *Pill style* (and saved in `settings.ini`):

- **Detailed** — glyph, capsule and percentage (the default).
- **Compact** — glyph and percentage, without the capsule.
- **Percent only** — just the percentage.

The pill is transparent by default; *Pill background* adds the Fluent taskbar plate. Hover paints
the same faint wash and hairline border as the Explorer tray buttons. Devices that stop answering
are removed rather than shown as disconnected.

### Panel

The panel keeps WattPill's own visual language: a 440 DIP card with a 20 px radius, the neutral
surface ladder (`#202020` panel, `#2B2B2B` cards), Inter for all text, 16 px card radius and the
violet forecast accent. It opens from the pill, centered on the widget, with a short slide and fade.

- The panel is pure content: no header, no close button, no gear. Settings open from the pill's
  right-click menu, so the flyout stays quiet.
- Each device is an 80 DIP row: a 22 px device glyph, the name in 14.5 px semibold, the status or
  forecast in 13.5 px (prefixed with the connection, e.g. `2.4 GHz · ≈ 4 ч`), the percentage in
  19 px semibold and a Fluent battery mark that morphs between levels. A small eye in the bottom
  corner hides the device from the pill; the row stays visible but desaturated, with a slashed eye
  to bring it back.
- Charging is green; at 20 % or below the glyph, battery mark, percentage and status turn red.
- Cards lift slightly on hover. The panel closes 200 ms after the pointer leaves it.

Light/dark is taken from the Windows app theme; the pill separately follows the taskbar theme.

## Type

- **Panel:** Inter (bundled, OFL) in three weights: text, medium, semibold. Registered privately at
  runtime, so the look does not depend on what is installed.
- **Pill:** Segoe UI Variable Text / Segoe UI Semibold at 12 px, rendered with **DirectWrite** — the
  same grayscale rasterizer WPF uses — so the digits stay crisp at taskbar size without a .NET
  runtime. The GDI+ path remains as a fallback.

DPI from 100 % to 200 % is handled by scaling every metric from the system DPI.

## Icons

- Panel device glyphs are Material Symbols Rounded (Apache-2.0); battery marks are Fluent System
  Icons (MIT). Both are embedded as PNG resources (`res/`), loaded once and tinted at draw time.
- The pill and the header gear use Segoe MDL2 Assets / Segoe Fluent Icons, which ship with Windows.

## Motion

- The panel fades in over about 120 ms while sliding 10 DIP toward its anchored position, like the
  shell's own flyouts.
- The charging battery breathes (a 33 ms timer that runs only while the panel is visible and only
  when something is actually charging).
- While the panel is closed and nothing changes, the app does **zero** drawing work.

## Settings

Left-click the pill opens the device panel; **right-click opens Settings** directly.

The settings panel has the same 440 DIP card, 20 px radius, Inter typography and neutral surfaces as
the device list:

- a compact header with the **WattPill** wordmark (20 px bold display face) on the left and a small
  *Настройки* caption on the right;
- one rounded card with a row per option, separated by a hairline;
- **toggle switches** for Start with Windows, Move widget, Pill background and Hide in fullscreen;
- **segmented buttons** for Placement, Pill style, Monitor and Language;
- a **Reset position** action row;
- two buttons at the bottom: **Refresh** (accent) and **Exit** (red).

There is no Save button: every change is applied and persisted immediately. The panel closes when
the pointer leaves it, so it never sits in the way.

## Layout

```
┌────────────────────────────────────────────┐
│  ┌──────────────────────────────────────┐  │
│  │  (icon)  Razer Barracuda X 2.4  48%  │  │
│  │          2.4 GHz · ≈ 3 дня           │  │
│  └──────────────────────────────────────┘  │
│  ┌──────────────────────────────────────┐  │
│  │  (icon)  SteelSeries Aerox 3    80%  │  │
│  │          2.4 GHz · ≈ 9 ч             │  │
│  └──────────────────────────────────────┘  │
└────────────────────────────────────────────┘
```

The panel is anchored to the center of the pill and placed above it (or below it when the taskbar is
at the top), inside the monitor's work area.

## Placement and anchoring

The pill is a normal topmost window, not a child of the taskbar. Position is computed from the
taskbar and notification-area rectangles on every guard tick:

- **On the taskbar** — vertically centered inside the bar, left of the tray with an 8 DIP gap.
- **Above the clock** — right-aligned with the tray, 4 DIP above the bar.
- **Manual** — Shift-drag moves the pill along the taskbar (offset from its right edge). Hold Ctrl,
  or enable *Move widget*, to drag it freely anywhere on the monitor (X and Y); the free position is
  stored relative to that monitor. *Reset position* returns to the default.

Hovering the pill shows a two-line rounded hint: *Shift — move along the taskbar* and
*Ctrl — move freely*.

The panel follows the same anchor, stays inside the monitor's work area and re-centers when the
pill moves.

## Taskbar z-order

The pill must sit above `Shell_TrayWnd` but must not use `SetParent`: layered child windows inside
the Windows 11 taskbar are swallowed by the shell's XAML island. Instead the app:

- keeps a 300 ms guard timer that re-asserts `HWND_TOPMOST` only when the taskbar really rose above
  the pill;
- reacts immediately to `EVENT_SYSTEM_FOREGROUND` through a WinEvent hook;
- re-anchors on `TaskbarCreated` (Explorer restart) and `WM_DISPLAYCHANGE`;
- hides the pill while the taskbar is auto-hidden or a fullscreen app is in front (optional).
  Fullscreen means the foreground root window is on the widget's own monitor, is visible and
  uncloaked, is not a shell window, and its window rectangle covers that monitor — the same rule
  PowerVision uses.

In steady state the guard makes no z-order calls and no repaints.

## Exit

A layered window's last surface can survive in the DWM composition on a translucent taskbar. On
exit the surface is wiped to fully transparent, the window is hidden and destroyed, then the
taskbar area is invalidated and `DwmFlush` is called, so nothing is left behind.
