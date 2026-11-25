#pragma once
#include "core.h"

namespace ch {

struct Settings {
    int pollSeconds = 60;
    bool showPill = true;
    bool dimSleeping = true;
    bool autoStart = true;
    int pillPlacement = 0;   // 0 above the clock, 1 on the taskbar
    bool pillManual = false; // the user dragged the pill
    int pillOffset = 12;     // DIP from the taskbar's right edge when pillManual
    bool pillUnlocked = false; // "Move widget": drag the pill without holding Shift
    int pillMode = 0;          // 0 detailed, 1 compact, 2 percent only
    bool pillBackground = false; // draw the Fluent taskbar plate behind the pill
    bool hideFullscreen = true;  // hide the widget in fullscreen apps
    int language = 0;   // 0 auto, 1 ru, 2 en
};

struct Paths {
    std::wstring baseDir;       // settings live here
    std::wstring dataDir;       // battery history lives here
    std::wstring settingsFile;
    std::wstring historyFile;
};

// `overrideDir` (--data) keeps everything in one folder, used by tests and portable runs.
Paths ResolvePaths(const std::wstring& overrideDir = L"");
bool EnsureDirectory(const std::wstring& dir);

Settings LoadSettings(const std::wstring& file);
bool SaveSettings(const std::wstring& file, const Settings& settings);

bool TaskbarIsLight();   // follows the Windows light/dark setting (read-only)
bool AppIsLight();       // apps vs system theme; the flyout follows this one
COLORREF SystemAccentColor(bool light);   // Windows accent palette, with a Fluent fallback

} // namespace ch
