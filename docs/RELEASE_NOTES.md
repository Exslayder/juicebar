# JuiceBar 1.0.1

Battery level **and remaining time** for wireless mice, headsets and controllers — right on the
Windows taskbar, with no vendor software.

## Install

Download `JuiceBar-1.0.1-setup.exe` and run it. The installer is per-user: no administrator
rights, no UAC prompt. It installs to `%LOCALAPPDATA%\Programs\JuiceBar` by default (the wizard
lets you choose another folder), adds a Start menu shortcut, and registers an uninstaller.

## Fixed

- **Borderless fullscreen games.** The pill and the panel now hide when the foreground window
  covers the monitor of the taskbar (and come back when you leave the game). Previously only
  exclusive fullscreen was detected, so a borderless game left the pill floating over it.
- **Empty devices for up to 80 seconds after a restart.** HID enumeration used to open every HID
  device on the system; one sleeping Bluetooth interface could block it for over a minute, so the
  first poll never completed and the panel said "no devices". Only known vendor IDs are opened now
  (80 s → ~16 ms), and an empty poll is retried after 5 s instead of a full minute.
- **Silent exit on startup.** A stale `ERROR_ALREADY_EXISTS` from an earlier API call could make
  the single-instance check think another copy was running; the last error is cleared first now.

## Changed

- **New device icons.** The pill and the panel use Material Symbols Rounded (Apache-2.0) for
  mice, headphones, keyboards, gamepads, speakers and phones; battery marks stay Fluent.
  Charging is still green, low battery red, a sleeping device dimmed.
- README: status badges, repository topics, and the installer's folder choice documented.
- The build is reproducible: the same commit produces a byte-identical `JuiceBar.exe`, and the
  pinned MinGW archive is verified by SHA-256 before extraction.

## Requirements

Windows 10 1903+ or Windows 11, x64. No runtime to install.

## Notes

- The pill lives next to the notification area on a horizontal taskbar.
- While the Start menu is open, Windows keeps its own shell window above every third-party
  overlay; the pill becomes clickable again as soon as the menu closes.
- Settings: `%APPDATA%\JuiceBar\settings.ini`; history: `%LOCALAPPDATA%\JuiceBar\history.tsv`.
  Uninstalling keeps them; delete the folders to remove everything.
