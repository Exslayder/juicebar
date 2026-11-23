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
    void (*refresh)() = nullptr;
    void (*menu)() = nullptr;       // open the tray menu at the cursor
};

bool Init(HINSTANCE inst, Settings* settings, Hooks hooks, const std::wstring& dataDir);
void Shutdown();

void SetCards(std::vector<Card> cards, int64_t polledAt);
void SetSettings(const Settings& settings);

void Tick();               // cheap: z-order, taskbar geometry, auto-hide
void OnSystemSettingsChanged();
void ShowPanelNow();       // --panel and the tray menu

// Diagnostic: which UI font families were resolved (used by --fonts).
std::wstring FontReport();

} // namespace ui
} // namespace ch
