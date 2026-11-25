#pragma once
#include "config.h"
#include "core.h"
#include "estimate.h"

namespace ch {
namespace ui {

// What the UI needs for one device.
struct Card {
    Reading reading;
    Forecast forecast;
};

struct Hooks {
    void (*menu)() = nullptr;            // open the tray menu at the cursor
    void (*refresh)() = nullptr;         // poll the devices right now
    void (*settingsChanged)() = nullptr; // persist a position changed by dragging
};

bool Init(HINSTANCE inst, Settings* settings, Hooks hooks);
void Shutdown();

void SetCards(std::vector<Card> cards);
void SetSettings(const Settings& settings);

void Tick();               // cheap: z-order, taskbar geometry, auto-hide
void OnSystemSettingsChanged();
void ShowPanelNow();       // --panel and the tray menu
void TogglePanelNow();     // "Open panel" menu item

// Diagnostic: which UI font families were resolved (used by --fonts).
std::wstring FontReport();
std::wstring FullscreenReport();   // --fullscreen diagnostic

} // namespace ui
} // namespace ch
