# Design system

WattPill is a native Win32 application. Everything you see is painted by hand on a
premultiplied 32-bit surface and pushed to the screen with `UpdateLayeredWindow`, so both the pill
and the panel get real per-pixel transparency and rounded edges without a browser engine.

## Surfaces

- **Pill** — by default floats just above the clock, right-aligned with the notification area. Its
  right edge is fixed and it grows to the left as devices connect, so it stays clear of taskbar
  buttons, toolbars and widgets. *Placement → On the taskbar* puts it left of the tray instead.
  Transparent by default; the device glyph and the level sit on that surface.
  Hover paints a single very light inset rounded wash (white at 12/255 on a dark taskbar, black at
  10/255 on a light one), not a full-height block — the same visual weight as the Explorer tray
  hover. Clicking toggles the panel; right-clicking opens the menu.
- **Panel** — one compact opaque card with the Windows 11 flyout radius (20 px), no header, no
  footer, no close button. Each device is a row: name, remaining time, level, battery glyph.
- Light/dark is taken from the taskbar (`SystemUsesLightTheme`) on every theme change event; there
  is no theme picker.

## Type

- Inter (bundled, OFL) in three weights: text, medium, semibold. Registered privately at runtime,
  so the look does not depend on what is installed.
- The pill uses Segoe UI Semibold at taskbar scale (it hints better at 13 px than Inter); the panel
  uses Inter. DPI from 100 % to 200 % is handled by scaling every metric from the system DPI.

## Icons

- Device glyphs are Material Symbols Rounded (Apache-2.0); battery marks are Fluent System Icons
  (MIT). Both are embedded as PNG resources (`res/`), loaded once and tinted at draw time. No web
  fonts, no inline SVG.

## Motion

- The panel fades in over about 120 ms while it is painted; there is no scale animation.
- The charging battery breathes (a 33 ms timer that runs only while the panel is visible and only
  when something is actually charging).
- While the panel is closed and nothing changes, the app does **zero** drawing work.

## Layout

```
┌─────────────────────────────────────────┐
│ 🎧  Razer Barracuda X 2.4          49%  │
│     ≈ 4 ч 10 мин                        │
├─────────────────────────────────────────┤
│ 🖱  Aerox 3 Wireless               75%  │
│     ≈ 18 ч 48 мин                       │
└─────────────────────────────────────────┘
```

The panel is anchored to the pill (or to the tray when the pill sits on the taskbar) and closes
200 ms after the pointer leaves it (or after leaving the pill for anywhere else). Standard
`TrackMouseEvent` + a one-shot timer — no global hooks, no polling.

## Taskbar z-order

The pill is a layered top-level window, because layered **child** windows inside `Shell_TrayWnd`
cannot be clicked on the Windows 11 taskbar (the shell's XAML island sits above them and swallows
input) and Windows offers no supported way to pin a popup above its own topmost taskbar.

The pill therefore walks the z-order list once per second (read-only) and calls
`SetWindowPos(HWND_TOPMOST)` **only** when the taskbar is actually above it — which happens after a
screenshot or a taskbar focus change. In steady state the app makes no z-order calls at all and no
repaints.

## Exit

A layered window's last surface can survive in the DWM composition on a translucent taskbar. On
exit the surface is wiped to fully transparent, the window is hidden and destroyed, then the
taskbar area is invalidated and `DwmFlush` is called, so nothing is left behind.
