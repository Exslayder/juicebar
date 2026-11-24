# Credits

WattPill is MIT licensed. It uses the following assets and protocol research.

## Fonts

- **Inter** by Rasmus Andersson — SIL Open Font License 1.1 (`fonts/OFL.txt`).
  Three static faces are embedded as Windows resources; the family names were changed to
  `ChargeHover Text / Medium / Semibold` so GDI+ can select the weight.

## Icons

- **Material Symbols Rounded** by Google — Apache-2.0,
  https://fonts.google.com/icons?selected=Material+Symbols+Rounded (device glyphs: mouse,
  headphones, keyboard, gamepad, speaker, phone)
- **Fluent System Icons** by Microsoft — MIT, https://github.com/microsoft/fluentui-system-icons
  (battery glyphs)
- The application icon is the Fluent `flash` glyph on a generated violet squircle.

## Runtime

WattPill has no runtime dependencies: the exe is a single self-contained Win32 + GDI+ binary. It
does not use WebView2, Electron or any embedded browser.

## Protocols

- **SteelSeries Aerox / Prime Wireless** battery command (`0x92` wired, `0xD2` over the receiver,
  reply layout, 5 % steps, charging bit): documented by the **rivalcfg** project (MIT) and confirmed
  on an Aerox 3 Wireless.
- **Razer Barracuda X (2022)** — Airoha RACE framing, YS-Tech CRC16/XMODEM and Macronix app mode,
  customer data `0x21` (percentage) and `0x2A` (cable state):
  - https://github.com/mehdibouchami/razer-barracuda-x-battery (MIT)
  - https://github.com/KhromotozzDevOut/razer-barracuda-battery-tray (MIT)
  - https://github.com/nightdevx/barracuda-x-battery (MIT)
- **Windows Bluetooth battery** — the device property `{104EA319-6EE2-4701-BD47-8DDBF425BBE5}, 2`
  that Windows itself maintains for headsets and LE devices.
- **HyperX Cloud II / Alpha / Flight** — the vendor HID report layouts (HP `0x03f0`, Kingston
  `0x0951`) as documented by the **HeadsetControl** project (GPL-3.0,
  https://github.com/Sapd/HeadsetControl). The protocol facts were re-implemented for this code
  base; no HeadsetControl code was copied.
- **HyperX Cloud Stinger Core Wireless + 7.1** (`0951:170B`) — the feature-report connection and
  battery queries, charge codes and voltage curves from **HyperBatteryMonitor** (MIT,
  https://github.com/davidsantanaalves/HyperBatteryMonitor), re-implemented for this code base.
- **Sony PULSE 3D** (`054C:0D5E`) — the pushed `0xB0` status report (level, charging state,
  connection flag) as documented by **HeadsetControl** (GPL-3.0) and **pulse3d-battery** (MIT,
  https://github.com/Ashraf-Obay/pulse3d-battery), re-implemented for this code base.
- **XInput** battery information via the documented `XInputGetBatteryInformation` API.

## Inspiration

- **BatteryHub** (`krisgfx9/Wireless-Device-Battery-Monitor`, GPL-3.0) for the feature comparison and
  the confirmation that per-device percentages are readable on the taskbar. No code was copied;
  protocols were implemented from the sources above and verified against real hardware.
- Design references: Apple HIG (grouped lists, restraint), Spotify (dark surface ladder),
  Linear and Raycast design notes from `VoltAgent/awesome-design-md` (MIT).
