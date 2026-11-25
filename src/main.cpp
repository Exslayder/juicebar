// WattPill: battery level and remaining time for wireless mice, headsets and controllers.
#include "config.h"
#include "core.h"
#include "drivers.h"
#include "estimate.h"
#include "history.h"
#include "jb_version.h"
#include "ui.h"

#include <propidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ch {
namespace {

const wchar_t* kAppName = L"WattPill";
const wchar_t* kVersion = JB_VERSION_WIDE;
const wchar_t* kWindowClass = L"WattPillMsg";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"WattPill";
const wchar_t* kMutexName = L"Local\\WattPillInstance";

constexpr UINT WM_APP_POLL = WM_APP + 1;
constexpr UINT WM_APP_SHOW = WM_APP + 2;

constexpr UINT_PTR TIMER_TICK = 1;
constexpr UINT_PTR TIMER_SAVE = 2;

enum MenuId : UINT {
    ID_REFRESH = 100,
    ID_PANEL = 130,
    ID_AUTOSTART = 150,
    ID_QUIT = 160,
    ID_PLACE_CLOCK = 170,
    ID_PLACE_TASKBAR = 171,
    ID_PLACE_RESET = 172,
    ID_MOVE = 173,
    ID_MODE_DETAILED = 174,
    ID_MODE_COMPACT = 175,
    ID_MODE_PERCENT = 176,
    ID_PILL_BG = 177,
    ID_HIDE_FULLSCREEN = 178,
    ID_MONITOR_FIRST = 200,
    ID_LANG_AUTO = 180,
    ID_LANG_RU = 181,
    ID_LANG_EN = 182,
};

struct Options {
    std::wstring dataDir;
    bool once = false;
    bool json = false;
    bool dump = false;
    bool showPanel = false;
    bool quit = false;
    bool noPill = false;
    bool fonts = false;
    bool fullscreen = false;
};

struct DeviceState {
    Reading last;
    int64_t lastSeen = 0;
};

struct App {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    bool exiting = false;

    Options options;
    Paths paths;
    Settings settings;
    bool savedShowPill = true;   // the file value, so --no-pill is not persisted
    History history;
    std::map<std::wstring, DeviceState> devices;
    std::vector<ui::Card> cards;

    HANDLE thread = nullptr;
    HANDLE wakeEvent = nullptr;
    HANDLE stopEvent = nullptr;
    std::atomic<int> pollSeconds{ 60 };
} app;

bool Ru() { return app.settings.language == 1 || (app.settings.language == 0 && RussianUi()); }
const wchar_t* T(const wchar_t* ru, const wchar_t* en) { return Ru() ? ru : en; }

// A GUI-subsystem binary has no console of its own: borrow the parent terminal for --once and
// --dump, and write UTF-8 straight to the inherited stdout handle so pipes keep working too.
void EnsureConsole() {
    if (GetConsoleWindow()) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    SetConsoleOutputCP(CP_UTF8);
}

void OutUtf8(const std::string& text) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(out, text.data(), (DWORD)text.size(), &written, nullptr);
}

void Out(const std::wstring& text) {
    OutUtf8(Utf8(text));
}


// ---------------------------------------------------------------- autostart

std::wstring AutostartCommand() {
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    return std::wstring(L"\"") + exe + L"\"";
}

void EnsureAutostart(bool on) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return;
    wchar_t buf[MAX_PATH * 3] = {};
    DWORD size = sizeof(buf), type = 0;
    const bool present = RegQueryValueExW(key, kRunValue, nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS;
    const std::wstring want = AutostartCommand();
    if (on && (!present || want != buf)) {
        RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)want.c_str(),
                       (DWORD)((want.size() + 1) * sizeof(wchar_t)));
    } else if (!on && present) {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

// The old JuiceBar autostart value would start the previous app alongside WattPill.
void RemoveLegacyAutostart() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"JuiceBar");
        RegCloseKey(key);
    }
}

// ---------------------------------------------------------------- menu

struct MonitorLite {
    RECT rect{};
    bool primary = false;
};

std::vector<MonitorLite> ListMonitors() {
    std::vector<MonitorLite> monitors;
    EnumDisplayMonitors(nullptr, nullptr,
                        [](HMONITOR mon, HDC, LPRECT, LPARAM data) -> BOOL {
                            auto* out = reinterpret_cast<std::vector<MonitorLite>*>(data);
                            MONITORINFO mi{ sizeof(mi) };
                            if (GetMonitorInfoW(mon, &mi)) {
                                MonitorLite entry;
                                entry.rect = mi.rcMonitor;
                                entry.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                                out->push_back(entry);
                            }
                            return TRUE;
                        },
                        (LPARAM)&monitors);
    std::stable_sort(monitors.begin(), monitors.end(), [](const MonitorLite& a, const MonitorLite& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.rect.left != b.rect.left) return a.rect.left < b.rect.left;
        return a.rect.top < b.rect.top;
    });
    return monitors;
}

void ShowMenu(HWND owner) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"WattPill");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_REFRESH, T(L"Обновить", L"Refresh"));
    AppendMenuW(menu, MF_STRING | (app.settings.autoStart ? MF_CHECKED : MF_UNCHECKED), ID_AUTOSTART,
                T(L"Автозагрузка", L"Start with Windows"));

    HMENU placement = CreatePopupMenu();
    AppendMenuW(placement, MF_STRING | (app.settings.pillPlacement == 0 ? MF_CHECKED : 0), ID_PLACE_CLOCK,
                T(L"Над часами", L"Above the clock"));
    AppendMenuW(placement, MF_STRING | (app.settings.pillPlacement == 1 ? MF_CHECKED : 0), ID_PLACE_TASKBAR,
                T(L"На панели задач", L"On the taskbar"));
    AppendMenuW(placement, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(placement, MF_STRING | (app.settings.pillUnlocked ? MF_CHECKED : 0), ID_MOVE,
                T(L"Переместить виджет", L"Move widget"));
    AppendMenuW(placement, MF_STRING, ID_PLACE_RESET, T(L"Сбросить позицию", L"Reset position"));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)placement, T(L"Расположение", L"Placement"));

    HMENU style = CreatePopupMenu();
    AppendMenuW(style, MF_STRING | (app.settings.pillMode == 0 ? MF_CHECKED : 0), ID_MODE_DETAILED,
                T(L"Подробно: значок, батарея, процент", L"Detailed: icon, battery, percent"));
    AppendMenuW(style, MF_STRING | (app.settings.pillMode == 1 ? MF_CHECKED : 0), ID_MODE_COMPACT,
                T(L"Компактно: значок и процент", L"Compact: icon and percent"));
    AppendMenuW(style, MF_STRING | (app.settings.pillMode == 2 ? MF_CHECKED : 0), ID_MODE_PERCENT,
                T(L"Только процент", L"Percent only"));
    AppendMenuW(style, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(style, MF_STRING | (app.settings.pillBackground ? MF_CHECKED : 0), ID_PILL_BG,
                T(L"Фон пилюли", L"Pill background"));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)style, T(L"Вид пилюли", L"Pill style"));
    AppendMenuW(menu, MF_STRING | (app.settings.hideFullscreen ? MF_CHECKED : 0), ID_HIDE_FULLSCREEN,
                T(L"Скрывать в полноэкранном режиме", L"Hide in fullscreen"));

    HMENU monitors = CreatePopupMenu();
    const auto monitorList = ListMonitors();
    if (monitorList.empty()) {
        AppendMenuW(monitors, MF_STRING | MF_DISABLED, 0, T(L"Мониторы не найдены", L"No monitors"));
    } else {
        const int nonPrimary = (int)monitorList.size() -
                               (monitorList[0].primary ? 1 : 0);
        int extra = 0;
        for (size_t i = 0; i < monitorList.size() && i < 16; i++) {
            const MonitorLite& m = monitorList[i];
            std::wstring label;
            if (m.primary) {
                label = T(L"Основной", L"Primary");
            } else {
                ++extra;
                label = T(L"Дополнительный", L"Additional");
                if (nonPrimary > 1) label += Fmt(L" %d", extra);
            }
            AppendMenuW(monitors, MF_STRING | (app.settings.monitorIndex == (int)i ? MF_CHECKED : 0),
                        ID_MONITOR_FIRST + (UINT)i, label.c_str());
        }
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)monitors, T(L"Монитор", L"Monitor"));

    HMENU language = CreatePopupMenu();
    AppendMenuW(language, MF_STRING | (app.settings.language == 0 ? MF_CHECKED : 0), ID_LANG_AUTO,
                T(L"Авто", L"Auto"));
    AppendMenuW(language, MF_STRING | (app.settings.language == 1 ? MF_CHECKED : 0), ID_LANG_RU,
                L"Русский");
    AppendMenuW(language, MF_STRING | (app.settings.language == 2 ? MF_CHECKED : 0), ID_LANG_EN,
                T(L"Английский", L"English"));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)language, T(L"Язык", L"Language"));

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_QUIT, T(L"Выход", L"Exit"));

    POINT p{};
    GetCursorPos(&p);
    SetForegroundWindow(owner);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_LEFTBUTTON, p.x, p.y, 0, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void SaveSettingsNow() {
    Settings saved = app.settings;
    if (app.options.noPill) saved.showPill = app.savedShowPill;   // a run-time switch, not a setting
    SaveSettings(app.paths.settingsFile, saved);
}

// Called from a driver's reader thread when a device pushed a new status.
void WakePoll() {
    if (app.wakeEvent) SetEvent(app.wakeEvent);
}

void HandleCommand(UINT id) {
    if (id == ID_REFRESH) {
        SetEvent(app.wakeEvent);
        return;
    }
    if (id == ID_PANEL) {
        ui::TogglePanelNow();
        return;
    }
    if (id == ID_AUTOSTART) {
        app.settings.autoStart = !app.settings.autoStart;
        EnsureAutostart(app.settings.autoStart);
        SaveSettingsNow();
        return;
    }
    if (id == ID_PLACE_CLOCK || id == ID_PLACE_TASKBAR) {
        app.settings.pillPlacement = id == ID_PLACE_TASKBAR ? 1 : 0;
        app.settings.pillFree = false;
        SaveSettingsNow();
        ui::SetSettings(app.settings);
        return;
    }
    if (id == ID_PLACE_RESET) {
        app.settings.pillManual = false;
        app.settings.pillOffset = 12;
        app.settings.pillFree = false;
        SaveSettingsNow();
        ui::SetSettings(app.settings);
        return;
    }
    if (id == ID_MOVE) {
        app.settings.pillUnlocked = !app.settings.pillUnlocked;
        SaveSettingsNow();
        ui::SetSettings(app.settings);
        return;
    }
    if (id == ID_MODE_DETAILED || id == ID_MODE_COMPACT || id == ID_MODE_PERCENT) {
        app.settings.pillMode = id == ID_MODE_DETAILED ? 0 : id == ID_MODE_COMPACT ? 1 : 2;
        SaveSettingsNow();
        ui::SetSettings(app.settings);
        return;
    }
    if (id == ID_PILL_BG) {
        app.settings.pillBackground = !app.settings.pillBackground;
        SaveSettingsNow();
        ui::SetSettings(app.settings);
        return;
    }
    if (id == ID_HIDE_FULLSCREEN) {
        app.settings.hideFullscreen = !app.settings.hideFullscreen;
        SaveSettingsNow();
        ui::SetSettings(app.settings);
        return;
    }
    if (id >= ID_MONITOR_FIRST && id < ID_MONITOR_FIRST + 16) {
        app.settings.monitorIndex = (int)(id - ID_MONITOR_FIRST);
        SaveSettingsNow();
        ui::SetSettings(app.settings);
        return;
    }
    if (id == ID_LANG_AUTO || id == ID_LANG_RU || id == ID_LANG_EN) {
        app.settings.language = id == ID_LANG_RU ? 1 : id == ID_LANG_EN ? 2 : 0;
        SetUiLanguage(app.settings.language);
        SaveSettingsNow();
        ui::SetSettings(app.settings);
        return;
    }
    if (id == ID_QUIT) PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------- polling

void BuildCards(const std::vector<Reading>& fresh, int64_t now) {
    std::map<std::wstring, bool> seen;
    for (const auto& reading : fresh) {
        auto& state = app.devices[reading.id];
        state.last = reading;
        state.lastSeen = now;
        seen[reading.id] = true;
        if (state.last.percent < 0) {
            // A sleeping device has no fresh level: show the last one we recorded, dimmed.
            auto samples = app.history.Get(reading.id, now - 30LL * 24 * 3600);
            if (!samples.empty()) state.last.percent = samples.back().percent;
        }
        app.history.Add(reading, now);
    }

    // Keep the last level for devices that stopped answering (asleep), like a receiver that drops
    // the link; a short grace keeps the number visible and dimmed instead of blinking out.
    const int64_t grace = 5 * 60;
    for (auto it = app.devices.begin(); it != app.devices.end();) {
        auto& state = it->second;
        if (seen.count(it->first)) {
            ++it;
            continue;
        }
        if (now - state.lastSeen > grace) {
            it = app.devices.erase(it);
            continue;
        }
        state.last.asleep = true;
        state.last.connected = false;
        state.last.charging = false;
        ++it;
    }

    std::vector<ui::Card> cards;
    for (const auto& [id, state] : app.devices) {
        if (!state.last.connected) continue;   // a device that stopped answering disappears
        ui::Card card;
        card.reading = state.last;
        if (state.last.asleep && !app.settings.dimSleeping) card.reading.asleep = false;
        std::vector<Sample> history = app.history.Get(id, now - 30LL * 24 * 3600);
        card.forecast = Estimate(history, card.reading.percent, card.reading.charging && !card.reading.asleep, now);
        cards.push_back(std::move(card));
    }

    std::stable_sort(cards.begin(), cards.end(), [](const ui::Card& a, const ui::Card& b) {
        if (a.reading.connected != b.reading.connected) return a.reading.connected;
        return a.reading.name < b.reading.name;
    });

    app.cards = std::move(cards);
    ui::SetCards(app.cards);
}

DWORD WINAPI PollThread(LPVOID) {
    HANDLE waits[2] = { app.wakeEvent, app.stopEvent };
    int emptyRetries = 0;
    for (;;) {
        // A full interval normally, but if the last read found nothing (common right after a
        // restart, while Windows re-enumerates the HID devices) retry shortly instead of leaving
        // the pill empty for a whole minute.
        DWORD timeout = (DWORD)(app.pollSeconds.load() * 1000);
        if (emptyRetries > 0) timeout = 5000;
        DWORD wait = WaitForMultipleObjects(2, waits, FALSE, timeout);
        if (wait == WAIT_OBJECT_0 + 1) break;
        if (wait == WAIT_TIMEOUT && app.exiting) break;

        PollReport* report = new PollReport(PollDevices());
        if (report->devices.empty()) {
            if (emptyRetries < 3) ++emptyRetries;
        } else {
            emptyRetries = 0;
        }
        if (!PostMessageW(app.hwnd, WM_APP_POLL, 0, (LPARAM)report)) delete report;
    }
    return 0;
}

// ---------------------------------------------------------------- window

// Native menus are always light unless the process opts into the undocumented uxtheme dark mode.
// The ordinals are stable from Windows 10 1809 through Windows 11; when they are missing the menu
// simply stays in the system light style.
void ApplyMenuTheme() {
    static HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
    if (!uxtheme) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
    using FlushMenuThemesFn = void(WINAPI*)();
    static auto setPreferredAppMode =
        (SetPreferredAppModeFn)GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
    static auto allowDarkModeForWindow =
        (AllowDarkModeForWindowFn)GetProcAddress(uxtheme, MAKEINTRESOURCEA(133));
    static auto flushMenuThemes = (FlushMenuThemesFn)GetProcAddress(uxtheme, MAKEINTRESOURCEA(136));

    const bool dark = !AppIsLight();
    if (setPreferredAppMode) setPreferredAppMode(dark ? 2 : 3);   // ForceDark / ForceLight
    if (app.hwnd) {
        if (allowDarkModeForWindow) allowDarkModeForWindow(app.hwnd, dark ? TRUE : FALSE);
        SetWindowTheme(app.hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }
    if (flushMenuThemes) flushMenuThemes();
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_POLL: {
            std::unique_ptr<PollReport> report(reinterpret_cast<PollReport*>(lp));
            if (!report) return 0;
            if (!report->log.empty() && app.options.once) OutputDebugStringW(report->log.c_str());
            BuildCards(report->devices, NowUnix());
            return 0;
        }
        case WM_APP_SHOW:
            if (app.cards.empty()) SetEvent(app.wakeEvent);
            ui::ShowPanelNow();
            return 0;
        case WM_TIMER:
            if (wp == TIMER_TICK) {
                ui::Tick();
            } else if (wp == TIMER_SAVE) {
                app.history.Save(app.paths.historyFile);
            }
            return 0;
        case WM_COMMAND:
            HandleCommand(LOWORD(wp));
            return 0;
        case WM_DEVICECHANGE:
            SetEvent(app.wakeEvent);   // a device was plugged in or paired: poll right away
            return 0;
        case WM_SETTINGCHANGE:
            ApplyMenuTheme();
            ui::OnSystemSettingsChanged();
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) SetEvent(app.wakeEvent);
            return 0;
        case WM_CLOSE:
            if (app.exiting) return 0;
            app.exiting = true;
            KillTimer(h, TIMER_TICK);
            KillTimer(h, TIMER_SAVE);
            SetEvent(app.stopEvent);
            if (app.thread) {
                WaitForSingleObject(app.thread, 3000);
                CloseHandle(app.thread);
                app.thread = nullptr;
            }
            app.history.Save(app.paths.historyFile);
            SaveSettingsNow();
            ui::Shutdown();
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- console modes

void PrintDeviceLine(const Reading& r) {
    const std::wstring percent = r.percent >= 0 ? Fmt(L"%3d%%", r.percent) : L"  ?%";
    const wchar_t* state = r.asleep ? L" (asleep)" : r.charging ? L" (charging)" : L"";
    Out(Fmt(L"  %-34s %s  [%s]%s\n", r.name.c_str(), percent.c_str(), r.via.c_str(), state));
}

std::string JsonString(const std::wstring& text) {
    const std::string utf8 = Utf8(text);
    std::string out = "\"";
    for (unsigned char c : utf8) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
    return out;
}

int RunOnce() {
    PollReport report = PollDevices();
    if (app.options.json) {
        std::string out = "[\n";
        for (size_t i = 0; i < report.devices.size(); i++) {
            const Reading& r = report.devices[i];
            out += "  {\"id\":" + JsonString(r.id) +
                   ",\"name\":" + JsonString(r.name) +
                   ",\"kind\":" + std::to_string((int)r.kind) +
                   ",\"percent\":" + std::to_string(r.percent) +
                   ",\"charging\":" + (r.charging ? "true" : "false") +
                   ",\"connected\":" + (r.connected ? "true" : "false") +
                   ",\"asleep\":" + (r.asleep ? "true" : "false") +
                   ",\"full\":" + (r.full ? "true" : "false") +
                   ",\"via\":" + JsonString(r.via) +
                   ",\"driver\":" + JsonString(r.driver) +
                   "}" + (i + 1 < report.devices.size() ? "," : "") + "\n";
        }
        out += "]\n";
        OutUtf8(out);
        return report.devices.empty() ? 1 : 0;
    }

    Out(Fmt(L"%s %s\n", kAppName, kVersion));
    for (const auto& r : report.devices) PrintDeviceLine(r);
    if (report.devices.empty()) Out(std::wstring(L"  ") + T(L"устройства не найдены", L"no devices found") + L"\n");
    if (!report.log.empty()) Out(report.log);
    return report.devices.empty() ? 1 : 0;
}

int RunDump() {
    std::wstring dump = DumpHid();
    Out(dump);
    return 0;
}

Options ParseOptions(int argc, wchar_t** argv) {
    Options o;
    for (int i = 1; i < argc; i++) {
        std::wstring arg = argv[i];
        if (arg == L"--once") o.once = true;
        else if (arg == L"--json") { o.json = true; o.once = true; }
        else if (arg == L"--dump") o.dump = true;
        else if (arg == L"--panel") o.showPanel = true;
        else if (arg == L"--quit") o.quit = true;
        else if (arg == L"--no-pill") o.noPill = true;
        else if (arg == L"--fonts") o.fonts = true;
        else if (arg == L"--fullscreen") o.fullscreen = true;
        else if (arg == L"--data" && i + 1 < argc) o.dataDir = argv[++i];
        else if (arg == L"--help" || arg == L"-h") {
            Out(Fmt(L"%s %s\n", kAppName, kVersion));
            Out(L"  --once           print devices and exit\n");
            Out(L"  --json           print devices as JSON and exit\n");
            Out(L"  --dump           list HID collections and exit\n");
            Out(L"  --fonts          report the UI fonts and exit\n");
            Out(L"  --fullscreen     report the fullscreen detection and exit\n");
            Out(L"  --panel          open the panel on start\n");
            Out(L"  --no-pill        start without the taskbar pill (this run)\n");
            Out(L"  --data <dir>     keep settings and history in <dir>\n");
            Out(L"  --quit           close the running instance\n");
            ExitProcess(0);
        }
    }
    return o;
}

} // namespace

int Run(int argc, wchar_t** argv) {
    Options options = ParseOptions(argc, argv);
    if (options.once || options.dump || options.fonts) EnsureConsole();
    if (options.dump) return RunDump();
    if (options.fonts) {
        Out(ui::FontReport());
        return 0;
    }
    if (options.fullscreen) {
        Out(ui::FullscreenReport());
        return 0;
    }

    // CreateMutexW only reports ERROR_ALREADY_EXISTS reliably when the last error is cleared
    // first; otherwise a stale error from an earlier call can make the app silently exit.
    SetLastError(0);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    const bool alreadyRunning = mutex && GetLastError() == ERROR_ALREADY_EXISTS;

    if (options.quit) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) PostMessageW(existing, WM_CLOSE, 0, 0);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    app.options = options;
    app.paths = ResolvePaths(options.dataDir);
    app.settings = LoadSettings(app.paths.settingsFile);
    SetUiLanguage(app.settings.language);
    app.savedShowPill = app.settings.showPill;
    if (options.dataDir.empty()) RemoveLegacyAutostart();
    EnsureAutostart(app.settings.autoStart);
    if (options.noPill) app.settings.showPill = false;
    app.pollSeconds = app.settings.pollSeconds;

    if (options.once) {
        if (alreadyRunning) { /* still fine: read-only query */ }
        if (mutex) CloseHandle(mutex);
        return RunOnce();
    }

    if (alreadyRunning) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) PostMessageW(existing, WM_APP_SHOW, 0, 0);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    app.inst = GetModuleHandleW(nullptr);
    app.history.Load(app.paths.historyFile);

    // Per-monitor DPI awareness, when the OS has it.
    using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto setCtx = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (!setCtx || !setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) SetProcessDPIAware();
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = app.inst;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    app.hwnd = CreateWindowExW(0, kWindowClass, kAppName, WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, app.inst, nullptr);
    if (!app.hwnd) return 1;
    ApplyMenuTheme();

    app.wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    app.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetPollWake(WakePoll);

    ui::Hooks hooks;
    hooks.menu = [] { ShowMenu(app.hwnd); };
    hooks.refresh = [] { SetEvent(app.wakeEvent); };
    hooks.settingsChanged = [] { SaveSettingsNow(); };
    ui::Init(app.inst, &app.settings, hooks);

    SetTimer(app.hwnd, TIMER_TICK, 300, nullptr);
    SetTimer(app.hwnd, TIMER_SAVE, 5 * 60 * 1000, nullptr);

    app.thread = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
    SetEvent(app.wakeEvent);   // first poll right away, not after one interval

    if (options.showPanel) {
        ui::ShowPanelNow();
        SetEvent(app.wakeEvent);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SetPollWake(nullptr);
    if (app.wakeEvent) CloseHandle(app.wakeEvent);
    if (app.stopEvent) CloseHandle(app.stopEvent);
    if (mutex) CloseHandle(mutex);
    return 0;
}

} // namespace ch

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    wchar_t dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return EXCEPTION_EXECUTE_HANDLER;
    std::wstring folder = std::wstring(dir) + L"\\WattPill";
    CreateDirectoryW(folder.c_str(), nullptr);
    const std::wstring dump = folder + L"\\crash.dmp";
    HANDLE file = CreateFileW(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{ GetCurrentThreadId(), info, FALSE };
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpWithThreadInfo, &mei, nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetUnhandledExceptionFilter(CrashFilter);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const int code = ch::Run(argc, argv);
    if (argv) LocalFree(argv);
    return code;
}
