# Promotion without a website

This file is for the maintainer. The app itself stays offline: no ads, no telemetry, no promotional
spinner in the tray. Everything below is either static (README, screenshots, a package manifest) or
a one-off GitHub action. Nothing runs on the user's PC.

## Already in place

- GitHub description and topics: `battery`, `taskbar`, `windows`, `win32`, `russian`, `english`,
  `localization`, `bilingual`, `i18n`, `sony`, `hyperx`, `razer`, `steelseries`.
- README badges, English screenshot with the panel and pill, a placement comparison, and the
  `UI: RU | EN` badge.
- `docs/social-preview.png` — upload it once in **Settings → Social preview**. GitHub will use it
  when the repository link is shared.
- Winget manifest in `packaging/winget/JuiceBar/JuiceBar/1.0.5/`; `winget validate` passes.

## Channels that need no new registration

### 1. Winget (best conversion)

Winget is built into Windows 10/11. A package in `microsoft/winget-pkgs` means:

```powershell
winget install JuiceBar.JuiceBar
```

Submitting uses the existing GitHub account and no local resources:

1. Fork `microsoft/winget-pkgs`.
2. Copy `packaging/winget/JuiceBar/JuiceBar/1.0.5/` to
   `manifests/j/JuiceBar/JuiceBar/1.0.5/`.
3. Run `winget validate <folder>` (already passes locally).
4. Open a PR with the manifest.

The manifest is ready; say the word and it can be submitted.

### 2. Awesome lists (GitHub PR)

These are curated GitHub lists; a PR uses the same account. Keep the entry short and follow each
list's rules:

- `awesome-windows` / `awesome-windows-apps` — under “Utilities” or “System”.
  Proposed line: `- [JuiceBar](https://github.com/Exslayder/juicebar) — native Windows taskbar pill with battery level and remaining time for wireless mice, headsets and controllers.`
- `awesome-battery` / similar hardware lists — only if the scope fits.
- Check the contribution rules first; do not add to lists that ban self-promotion.

### 3. GitHub only

- Release notes are already the announcement: link to the latest release from the README.
- A short “Show and tell” post in GitHub Discussions (if enabled) gives existing visitors a place
  to ask questions; it does not bring new users by itself.
- Ask a friend with a real account to star/share; do not use fake accounts.

## Copy-paste posts

Use only on platforms where you already have an account. Read the rules, disclose that you are the
author, and do not post the same text in several communities on the same day.

### Reddit — r/Windows11

**Title:** I made a native taskbar battery pill for wireless mice and headsets (no vendor software)

**Body:**
> I wanted the battery level of my wireless mouse and headset on the taskbar without installing
> Razer Synapse / SteelSeries GG / HyperX software. JuiceBar is a small native Win32 app: a pill
> above the clock shows the real percentage (or a 10 % step where the device only reports that) and
> an estimated time left. Click it for a compact panel.
>
> - No telemetry, no account, no WebView2/Electron; ~22 MB RAM, no idle CPU.
> - English and Russian UI; the first run follows Windows.
> - Placement: above the clock (default) or on the taskbar left of the tray.
> - Supports SteelSeries Aerox/Prime, Razer Barracuda X, HyperX Cloud/Stinger, Sony PULSE 3D,
>   Xbox controllers and Bluetooth devices whose battery Windows exposes.
>
> Installer (per-user, unsigned): https://github.com/Exslayder/juicebar/releases/latest
> Source and screenshots: https://github.com/Exslayder/juicebar
>
> It is my project; happy to answer questions and add devices if you send a HID dump.

### Reddit — r/software (shorter)

**Title:** JuiceBar — battery percentage and time left on the Windows taskbar

**Body:**
> A small native Windows tool that shows the real battery level and an estimated remaining time for
> wireless mice, headsets and controllers. No vendor software, no telemetry, no account; English and
> Russian UI. Per-user installer: https://github.com/Exslayder/juicebar/releases/latest
> (I am the author.)

### Hacker News — Show HN

**Title:** Show HN: JuiceBar – native Windows taskbar battery monitor

**Text:**
> I built a small Win32/GDI+ app that puts a battery pill on the Windows taskbar. It talks to
> wireless receivers directly (SteelSeries, Razer, HyperX, Sony PULSE 3D), falls back to the Windows
> Bluetooth battery property and XInput, and fits a local discharge curve for “time left”. No
> telemetry and no vendor software.
>
> Source and screenshots: https://github.com/Exslayder/juicebar
> Installer: https://github.com/Exslayder/juicebar/releases/latest
>
> The interesting part for me was the protocol work and the borderless-fullscreen detection; happy
> to discuss either.

### Telegram / X (short)

> JuiceBar — уровень заряда и оставшееся время прямо на панели задач Windows. Мыши, наушники,
> геймпады; без софта производителя, без телеметрии. Русский/English.
> https://github.com/Exslayder/juicebar/releases/latest

## Rules

- Disclose that you are the author.
- Read each community's self-promotion rules; use showcase threads where required.
- One post per community, answer comments, do not cross-post identical text on the same day.
- Do not buy stars/downloads and do not use fake accounts.
- Do not add ads, network calls or a promotional spinner to the app; it would break the
  “no telemetry / native / light” promise.

## Metrics

GitHub release downloads and stars are visible without a website:

```powershell
(Invoke-RestMethod -Headers @{ "User-Agent"="JuiceBar" } `
  -Uri "https://api.github.com/repos/Exslayder/juicebar/releases").assets |
  Select-Object name, download_count
```

Watch the first two weeks: downloads, stars, issues and which device families people ask for. Add
drivers only when a real dump/protocol is available.
