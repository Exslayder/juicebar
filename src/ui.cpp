// WattPill UI: a compact taskbar pill plus a hover panel with per-device battery
// forecast. Everything is drawn by hand on a premultiplied 32-bit surface and pushed with
// UpdateLayeredWindow, so the pill and the panel get real rounded corners and soft alpha.
#include "ui.h"
#include "dwrite_text.h"

#include <propidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <map>

using namespace Gdiplus;

namespace ch {
namespace ui {
namespace {


constexpr UINT WM_APP_REFRESH_UI = WM_APP + 40;

constexpr int kPanelTimerHide = 2;
constexpr int kPanelTimerAnim = 3;
constexpr int kPillTipTimer = 4;

struct Theme {
    // Flyout: WattPill's own neutral surface ladder with the violet forecast accent.
    Color bg{ 250, 23, 24, 29 };
    Color card{ 12, 255, 255, 255 };
    Color cardHover{ 24, 255, 255, 255 };
    Color border{ 26, 255, 255, 255 };
    Color text{ 255, 242, 243, 247 };
    Color sub{ 255, 150, 156, 170 };
    Color faint{ 255, 104, 110, 124 };
    Color accent{ 255, 124, 92, 255 };
    Color red{ 255, 248, 113, 113 };
    Color green{ 255, 74, 222, 128 };
    Color amber{ 255, 157, 93, 0 };
    Color charging{ 255, 0, 95, 184 };

    // Taskbar pill: the exact PowerVision / Fluent taskbar tokens.
    Color pillBg{ 1, 0, 0, 0 };
    Color pillBorder{ 0, 0, 0, 0 };
    Color pillHover{ 22, 255, 255, 255 };
    Color pillHoverBorder{ 34, 255, 255, 255 };
    Color pillInk{ 255, 255, 255, 255 };
    Color pillSub{ 255, 200, 200, 200 };
    Color battFrame{ 255, 184, 184, 184 };
    Color battTrack{ 36, 255, 255, 255 };
    Color battGood{ 255, 108, 203, 95 };
    Color battMid{ 255, 252, 225, 0 };
    Color battLow{ 255, 255, 153, 164 };
    Color battCharging{ 255, 96, 205, 255 };
};

struct Fonts {
    // The panel keeps WattPill's bundled Inter faces; the taskbar pill borrows the Windows 11
    // Segoe UI Semibold + Fluent Icons stack so it sits naturally next to the tray.
    std::unique_ptr<FontFamily> fallbackUi;
    std::unique_ptr<FontFamily> fallbackSemi;
    std::unique_ptr<FontFamily> iconFamily;
    std::unique_ptr<FontFamily> pillFamily;
    std::unique_ptr<FontFamily> displayFamily;
    std::unique_ptr<Font> name, small, status, percent;
    std::unique_ptr<Font> wordmark;
    std::unique_ptr<Font> pillPct, pillGlyph, pillBolt;

    void Build(float scale, bool hasUi, FontFamily* regular, FontFamily* medium, FontFamily* semibold) {
        if (!regular) {
            fallbackUi = std::make_unique<FontFamily>(hasUi ? L"Segoe UI Variable Text" : L"Segoe UI");
            if (fallbackUi->GetLastStatus() != Ok) fallbackUi = std::make_unique<FontFamily>(L"Segoe UI");
            fallbackSemi = std::make_unique<FontFamily>(L"Segoe UI Semibold");
            if (fallbackSemi->GetLastStatus() != Ok) fallbackSemi = std::make_unique<FontFamily>(L"Segoe UI");
            regular = fallbackUi.get();
            medium = fallbackUi.get();
            semibold = fallbackSemi.get();
        }

        iconFamily = std::make_unique<FontFamily>(L"Segoe MDL2 Assets");
        if (iconFamily->GetLastStatus() != Ok) iconFamily = std::make_unique<FontFamily>(L"Segoe Fluent Icons");
        if (iconFamily->GetLastStatus() != Ok) iconFamily = std::make_unique<FontFamily>(L"Segoe UI Symbol");

        auto make = [&](std::unique_ptr<Font>& f, int weight, float px) {
            FontFamily* family = weight == 2 ? semibold : weight == 1 ? medium : regular;
            f = std::make_unique<Font>(family, px * scale, FontStyleRegular, UnitPixel);
        };
        make(name, 2, 14.5f);
        make(status, 2, 13.5f);
        make(small, 0, 12.0f);
        make(percent, 2, 19.0f);

        displayFamily = std::make_unique<FontFamily>(L"Segoe UI Variable Display");
        if (displayFamily->GetLastStatus() != Ok) displayFamily = std::make_unique<FontFamily>(L"Segoe UI");
        wordmark = std::make_unique<Font>(displayFamily.get(), 20.0f * scale, FontStyleBold, UnitPixel);

        pillFamily = std::make_unique<FontFamily>(L"Segoe UI Semibold");
        if (pillFamily->GetLastStatus() != Ok) pillFamily = std::make_unique<FontFamily>(L"Segoe UI");
        pillPct = std::make_unique<Font>(pillFamily.get(), 12.0f * scale, FontStyleRegular, UnitPixel);
        pillGlyph = std::make_unique<Font>(iconFamily.get(), 13.5f * scale, FontStyleRegular, UnitPixel);
        pillBolt = std::make_unique<Font>(iconFamily.get(), 9.5f * scale, FontStyleRegular, UnitPixel);
    }
};

struct BundledFont {
    std::vector<BYTE> bytes;
    bool Load(HINSTANCE inst, int id) {
        HRSRC res = FindResourceW(inst, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!res) return false;
        const DWORD size = SizeofResource(inst, res);
        HGLOBAL handle = LoadResource(inst, res);
        if (!size || !handle) return false;
        const void* data = LockResource(handle);
        if (!data) return false;
        bytes.assign((const BYTE*)data, (const BYTE*)data + size);
        return true;
    }
};

enum IconId : int {
    icoBatteryCharge = 310,
    icoHeadphonesFill = 341, icoKeyboardFill, icoGamepadFill, icoSpeakerFill, icoPhoneFill, icoMouseFill,
    icoBattery0 = 320,
};

// Ready-made Fluent System Icons (MIT) and Lucide (MIT), rendered to PNG at build time.
// They are tinted at draw time with a colour matrix, so one asset serves every state.
struct IconStore {
    std::map<int, std::unique_ptr<Bitmap>> images;

    void Load(HINSTANCE inst, int id) {
        HRSRC res = FindResourceW(inst, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!res) return;
        const DWORD size = SizeofResource(inst, res);
        HGLOBAL handle = LoadResource(inst, res);
        if (!size || !handle) return;
        const void* data = LockResource(handle);
        if (!data) return;
        HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!copy) return;
        void* target = GlobalLock(copy);
        if (!target) { GlobalFree(copy); return; }
        memcpy(target, data, size);
        GlobalUnlock(copy);
        IStream* stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(copy, TRUE, &stream)) || !stream) { GlobalFree(copy); return; }
        auto bitmap = std::make_unique<Bitmap>(stream);
        stream->Release();
        if (bitmap->GetLastStatus() == Ok) images[id] = std::move(bitmap);
    }

    Bitmap* Get(int id) const {
        auto it = images.find(id);
        return it == images.end() ? nullptr : it->second.get();
    }

    void Draw(Graphics& g2, int id, RectF rect, Color tint, BYTE alpha = 255) {
        Bitmap* bitmap = Get(id);
        if (!bitmap) return;
        const float a = alpha / 255.0f;
        ColorMatrix matrix = {
            0, 0, 0, 0, 0,
            0, 0, 0, 0, 0,
            0, 0, 0, 0, 0,
            0, 0, 0, a, 0,
            tint.GetR() / 255.0f, tint.GetG() / 255.0f, tint.GetB() / 255.0f, 0, 1,
        };
        ImageAttributes attributes;
        attributes.SetColorMatrix(&matrix);
        g2.DrawImage(bitmap, rect, 0, 0, (INT)bitmap->GetWidth(), (INT)bitmap->GetHeight(), UnitPixel, &attributes);
    }
};

enum class PanelMode { Devices, Settings };

enum SettingId {
    kSetAutostart = 1,
    kSetMove,
    kSetBackground,
    kSetFullscreen,
    kSetPlacement,
    kSetStyle,
    kSetMonitor,
    kSetLanguage,
    kSetReset,
    kSetRefresh,
    kSetExit,
};

struct SettingHit {
    int id = 0;
    int option = 0;
    RectF rect;
};

struct State {
    HINSTANCE inst = nullptr;
    Settings* settings = nullptr;
    Hooks hooks;
    HWND pill = nullptr;
    HWND panel = nullptr;

    std::vector<Card> cards;

    bool pillHover = false;
    bool visible = false;

    int hoveredCard = -1;
    int hoveredEye = -1;
    std::vector<RectF> eyeRects;
    PanelMode panelMode = PanelMode::Devices;
    std::vector<SettingHit> settingHits;
    int hoveredSetting = -1;

    float scale = 1.0f;
    Theme theme;
    Fonts fonts;
    IconStore icons;
    BundledFont fontData[3];
    std::unique_ptr<PrivateFontCollection> fontCollection;
    FontFamily* familyRegular = nullptr;
    FontFamily* familyMedium = nullptr;
    FontFamily* familySemibold = nullptr;
    std::map<std::wstring, float> ringValues;
    float spin = 0;
    float reveal = 0;
    ULONGLONG animLast = 0;
    POINT panelPos{};
    RECT pillRect{};
    std::vector<RectF> cardRects;
    bool fullscreen = false;
    bool taskbarHidden = false;

    bool dragArmed = false;
    bool dragMoved = false;
    bool dragFree = false;
    int dragStartX = 0;
    int dragStartY = 0;
    int dragStartLeft = 0;
    int dragStartTop = 0;

    HWND tip = nullptr;
    bool tipVisible = false;
    int tipW = 0;
    int tipH = 0;

    HWINEVENTHOOK winEventHook = nullptr;
    UINT taskbarCreatedMessage = 0;

    bool uiFontVariable = true;
} g;

// ------------------------------------------------------------------ helpers

const wchar_t* Tr(const wchar_t* ru, const wchar_t* en) {
    if (!g.settings) return en;
    int lang = g.settings->language;
    bool ruUi = lang == 1 || (lang == 0 && RussianUi());
    return ruUi ? ru : en;
}

int IconFor(Kind kind) {
    switch (kind) {
        case Kind::Mouse: return icoMouseFill;
        case Kind::Keyboard: return icoKeyboardFill;
        case Kind::Headset: return icoHeadphonesFill;
        case Kind::Earbuds: return icoHeadphonesFill;
        case Kind::Gamepad: return icoGamepadFill;
        case Kind::Speaker: return icoSpeakerFill;
        case Kind::Phone: return icoPhoneFill;
        default: return icoBatteryCharge;
    }
}

Color WithAlpha(Color c, BYTE alpha) { return Color(alpha, c.GetR(), c.GetG(), c.GetB()); }

float TextWidth(Graphics& g, const std::wstring& s, Font* font) {
    if (s.empty()) return 0;
    RectF bounds;
    StringFormat fmt;
    fmt.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    g.MeasureString(s.c_str(), (INT)s.size(), font, RectF(0, 0, 2000, 200), &fmt, &bounds);
    return bounds.Width;
}

void Text(Graphics& g, const std::wstring& s, Font* font, RectF box, Color color, StringAlignment align,
          bool ellipsis = false) {
    if (s.empty()) return;
    StringFormat fmt;
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    if (ellipsis) fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush brush(color);
    g.DrawString(s.c_str(), (INT)s.size(), font, box, &fmt, &brush);
}

void RoundRect(GraphicsPath& path, RectF r, float radius) {
    float d = std::min(radius * 2, std::min(r.Width, r.Height));
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

void FillRound(Graphics& g, RectF r, float radius, Color color) {
    GraphicsPath path;
    RoundRect(path, r, radius);
    SolidBrush brush(color);
    g.FillPath(&brush, &path);
}

void StrokeRound(Graphics& g, RectF r, float radius, Color color, float width) {
    GraphicsPath path;
    RoundRect(path, r, radius);
    Pen pen(color, width);
    g.DrawPath(&pen, &path);
}

// The wordmark's battery mark, drawn as a shape so it matches the app icon at every DPI.
struct Canvas {
    int w = 0, h = 0;
    std::unique_ptr<Bitmap> bitmap;
    std::unique_ptr<Graphics> g;

    Canvas(int width, int height) : w(width), h(height) {
        bitmap = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
        g = std::make_unique<Graphics>(bitmap.get());
        g->SetSmoothingMode(SmoothingModeAntiAlias);
        g->SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g->SetPixelOffsetMode(PixelOffsetModeHalf);
        g->Clear(Color(0, 0, 0, 0));
    }

    void Present(HWND wnd, int x, int y, BYTE alpha = 255) {
        BitmapData data{};
        Rect rect(0, 0, w, h);
        if (bitmap->LockBits(&rect, ImageLockModeRead, PixelFormat32bppPARGB, &data) != Ok) return;

        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib && bits) {
            for (int row = 0; row < h; row++) {
                memcpy((BYTE*)bits + (size_t)row * w * 4, (BYTE*)data.Scan0 + (size_t)row * data.Stride, (size_t)w * 4);
            }
            HGDIOBJ old = SelectObject(mem, dib);
            POINT dst{ x, y }, src{ 0, 0 };
            SIZE size{ w, h };
            BLENDFUNCTION blend{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
            UpdateLayeredWindow(wnd, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
            SelectObject(mem, old);
        }
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        bitmap->UnlockBits(&data);
    }
};

// ------------------------------------------------------------------ layout & status text

std::wstring PercentText(const Card& card) {
    if (!card.reading.connected) return L"—";
    if (card.reading.percent < 0) return L"?";
    return Fmt(L"%d%%", card.reading.percent);
}

bool IsLow(const Card& card) {
    return card.reading.connected && card.reading.percent >= 0 && card.reading.percent <= 20 && !card.reading.charging;
}

bool HiddenFromPill(const std::wstring& id) {
    return std::find(g.settings->hiddenDevices.begin(), g.settings->hiddenDevices.end(), id) !=
           g.settings->hiddenDevices.end();
}

Color PercentColor(const Card& card) {
    if (IsLow(card)) return g.theme.red;
    if (card.reading.charging) return g.theme.green;
    if (card.reading.asleep || !card.reading.connected) return g.theme.faint;
    return g.theme.text;
}

std::wstring FormatForecast(int minutes) {
    if (minutes < 0) return L"";
    if (minutes >= 48 * 60) {
        int days = (int)std::lround(minutes / (24.0 * 60.0));
        days = std::max(2, days);
        if (RussianUi()) {
            const int mod100 = days % 100, mod10 = days % 10;
            const wchar_t* word = (mod100 >= 11 && mod100 <= 14) ? L"дней"
                                  : mod10 == 1 ? L"день"
                                  : (mod10 >= 2 && mod10 <= 4) ? L"дня"
                                  : L"дней";
            return Fmt(L"%d %s", days, word);
        }
        return Fmt(L"%d day%s", days, days == 1 ? L"" : L"s");
    }
    return FormatSpan(minutes);
}

std::wstring StatusLine(const Card& card) {
    const Reading& r = card.reading;
    const Forecast& f = card.forecast;
    if (!r.connected) return Tr(L"Не подключено", L"Not connected");
    if (r.asleep && r.percent < 0) return Tr(L"Ждём первый замер", L"Waiting for the first reading");
    if (r.asleep) return Tr(L"Нет ответа · последний замер", L"No answer · last reading");

    const std::wstring prefix = r.via.empty() ? L"" : (r.via + L" · ");

    if (r.charging) {
        if (f.state == ForecastState::Charging && f.minutes >= 0)
            return prefix + Tr(L"Заряжается · до полного ", L"Charging · ") + FormatSpan(f.minutes);
        if (r.full || r.percent >= 100) return prefix + Tr(L"Полностью заряжено", L"Fully charged");
        return prefix + Tr(L"Заряжается", L"Charging");
    }

    switch (f.state) {
        case ForecastState::Ready:
        case ForecastState::LowConfidence:
            return prefix + L"≈ " + FormatForecast(f.minutes);
        case ForecastState::Flat:
            return prefix + Tr(L"Расход стабилен", L"Drain is steady");
        case ForecastState::Learning:
            return prefix + Tr(L"Мало данных для прогноза", L"Not enough data yet");
        case ForecastState::NoData:
        default:
            return prefix + Tr(L"Ожидание данных", L"Waiting for data");
    }
}

// ------------------------------------------------------------------ drawing: pill

struct PillItem {
    const Card* card = nullptr;
    float x = 0;
    float glyphW = 0;
    float capW = 0;
    float boltW = 0;
    float boltX = -1;
    float textX = 0;
    float textW = 0;
};

const wchar_t* KindGlyph(Kind kind) {
    switch (kind) {
        case Kind::Mouse: return L"\uE962";
        case Kind::Keyboard: return L"\uE765";
        case Kind::Headset: return L"\uE7F6";
        case Kind::Earbuds: return L"\uE7F6";
        case Kind::Gamepad: return L"\uE7FC";
        case Kind::Speaker: return L"\uE7F5";
        case Kind::Phone: return L"\uE8EA";
        default: return L"\uE702";
    }
}

Color PillTier(const Card& card) {
    if (card.reading.charging) return g.theme.battGood;
    if (card.reading.percent <= 20) return g.theme.battLow;
    if (card.reading.percent <= 45) return g.theme.battMid;
    return g.theme.battGood;
}

// ---------------------------------------------------------------- monitor/taskbar layout

struct MonitorEntry {
    HMONITOR handle = nullptr;
    RECT rect{};
    RECT work{};
    bool primary = false;
};

BOOL CALLBACK CollectMonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM data) {
    auto* list = reinterpret_cast<std::vector<MonitorEntry>*>(data);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    MonitorEntry entry;
    entry.handle = mon;
    entry.rect = mi.rcMonitor;
    entry.work = mi.rcWork;
    entry.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    list->push_back(entry);
    return TRUE;
}

std::vector<MonitorEntry> Monitors() {
    std::vector<MonitorEntry> list;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitorProc, (LPARAM)&list);
    std::stable_sort(list.begin(), list.end(), [](const MonitorEntry& a, const MonitorEntry& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.rect.left != b.rect.left) return a.rect.left < b.rect.left;
        return a.rect.top < b.rect.top;
    });
    return list;
}

HMONITOR MonitorAt(int index) {
    const auto list = Monitors();
    if (list.empty()) return MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    return list[(size_t)std::clamp(index, 0, (int)list.size() - 1)].handle;
}

struct BarLayout {
    bool found = false;
    HWND taskbar = nullptr;
    RECT monitor{}, work{}, bar{}, notify{};
    bool atTop = false;
    bool horizontal = true;
};

BOOL CALLBACK CollectBarProc(HWND hwnd, LPARAM data) {
    auto* pair = reinterpret_cast<std::pair<HMONITOR, BarLayout*>*>(data);
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (_wcsicmp(cls, L"Shell_TrayWnd") && _wcsicmp(cls, L"Shell_SecondaryTrayWnd")) return TRUE;
    if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != pair->first) return TRUE;
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) return TRUE;
    BarLayout& layout = *pair->second;
    layout.found = true;
    layout.taskbar = hwnd;
    layout.bar = rect;
    if (HWND notify = FindWindowExW(hwnd, nullptr, L"TrayNotifyWnd", nullptr))
        GetWindowRect(notify, &layout.notify);
    return FALSE;
}

BarLayout BarForMonitor(HMONITOR mon) {
    BarLayout layout;
    MONITORINFO mi{ sizeof(mi) };
    if (mon && GetMonitorInfoW(mon, &mi)) {
        layout.monitor = mi.rcMonitor;
        layout.work = mi.rcWork;
    } else {
        layout.monitor = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        layout.work = layout.monitor;
    }
    std::pair<HMONITOR, BarLayout*> data{ mon, &layout };
    EnumWindows(CollectBarProc, (LPARAM)&data);
    if (layout.found) {
        const int w = layout.bar.right - layout.bar.left;
        const int h = layout.bar.bottom - layout.bar.top;
        layout.horizontal = w >= h;
        layout.atTop = layout.bar.top < layout.monitor.top + (int)std::lround(40 * g.scale);
    }
    return layout;
}

// ------------------------------------------------------------------ hover hint

void HideTip() {
    if (g.tip && g.tipVisible) ShowWindow(g.tip, SW_HIDE);
    g.tipVisible = false;
}

void RenderTip() {
    if (!g.tip) return;
    const std::wstring line1 = Tr(L"Shift — двигать по панели", L"Shift — move along the taskbar");
    const std::wstring line2 = Tr(L"Ctrl — свободно по экрану", L"Ctrl — move freely");
    Canvas probe(4, 4);
    const float w1 = TextWidth(*probe.g, line1, g.fonts.small.get());
    const float w2 = TextWidth(*probe.g, line2, g.fonts.small.get());
    const float textW = std::max(w1, w2);
    const int padX = (int)std::lround(11 * g.scale);
    const int padY = (int)std::lround(7 * g.scale);
    const int lineH = (int)std::lround(16 * g.scale);
    const int gap = (int)std::lround(1 * g.scale);
    const int height = padY * 2 + lineH * 2 + gap;
    const int width = (int)std::lround(textW) + padX * 2;
    g.tipW = width;
    g.tipH = height;

    Canvas canvas(width, height);
    Graphics& g2 = *canvas.g;
    const int radius = (int)std::lround(8 * g.scale);
    FillRound(g2, RectF(0, 0, (float)width, (float)height), (float)radius, g.theme.card);
    StrokeRound(g2, RectF(0.5f, 0.5f, width - 1.0f, height - 1.0f), (float)radius, g.theme.border, 1.0f);
    Text(g2, line1, g.fonts.small.get(),
         RectF((float)padX, (float)padY, textW + 2, (float)lineH), g.theme.text, StringAlignmentNear);
    Text(g2, line2, g.fonts.small.get(),
         RectF((float)padX, (float)(padY + lineH + gap), textW + 2, (float)lineH),
         g.theme.text, StringAlignmentNear);

    const RECT pill = g.pillRect;
    int tipX = (pill.left + pill.right) / 2 - width / 2;
    int tipY = pill.top - height - (int)std::lround(8 * g.scale);
    const BarLayout layout = BarForMonitor(MonitorAt(g.settings->monitorIndex));
    const RECT mon = layout.monitor;
    if (tipY < mon.top + 4) tipY = pill.bottom + (int)std::lround(8 * g.scale);
    tipX = std::clamp(tipX, (int)mon.left + 4, (int)mon.right - width - 4);
    tipY = std::clamp(tipY, (int)mon.top + 4, (int)mon.bottom - height - 4);

    canvas.Present(g.tip, tipX, tipY);
}

void ShowTip() {
    if (!g.tip || g.fullscreen || g.taskbarHidden || g.dragArmed || g.visible) return;
    RenderTip();
    ShowWindow(g.tip, SW_SHOWNOACTIVATE);
    SetWindowPos(g.tip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g.tipVisible = true;
}

// The taskbar pill is a metric-for-metric native re-creation of PowerVision's widget:
// a 40 DIP window with a 38 DIP, radius-5 root; 8 DIP side padding; each device has a 5 DIP
// margin, a 13.5 DIP Fluent glyph, a 16x9.5 battery capsule, an optional 9.5 DIP bolt and 12 DIP
// semibold text.
void RenderPill() {
    if (!g.pill) return;

    const BarLayout layout = BarForMonitor(MonitorAt(g.settings->monitorIndex));
    RECT barRect = layout.bar, notifyRect = layout.notify;
    const int barW = barRect.right - barRect.left, barH = barRect.bottom - barRect.top;
    if (!g.settings->pillFree && layout.found && barW <= barH) return;   // vertical taskbar

    if (g.taskbarHidden) {
        if (!g.settings->pillFree) ShowWindow(g.pill, SW_HIDE);
        return;
    }

    const int mode = std::clamp(g.settings->pillMode, 0, 2);   // 0 detailed, 1 compact, 2 percent
    const bool showIcon = mode != 2;
    const bool showCapsule = mode == 0;

    const int height = (int)std::lround(40 * g.scale);
    const float padX = 8 * g.scale;
    const float margin = 5 * g.scale;
    const float iconGap = 5 * g.scale;
    const float glyphW = 13.5f * g.scale;
    const float capW = 16 * g.scale, capH = 9.5f * g.scale;
    const float capBorder = 1.1f * g.scale, capPad = 1.0f * g.scale;
    const float nubGap = 0.8f * g.scale, nubW = 1.6f * g.scale, nubH = 4.5f * g.scale;
    const float boltGap = 2.5f * g.scale, textGap = 4 * g.scale;
    const float rootY = 1 * g.scale, rootH = 38 * g.scale, radius = 5 * g.scale;

    std::vector<const Card*> shown;
    for (const auto& card : g.cards)
        if (card.reading.connected && !HiddenFromPill(card.reading.id)) shown.push_back(&card);

    Canvas probe(4, 4);
    Graphics& pg = *probe.g;
    const float boltAdvance = TextWidth(pg, L"\uE945", g.fonts.pillBolt.get());

    std::vector<PillItem> items;
    float x = padX + margin;
    for (size_t i = 0; i < shown.size(); i++) {
        const Card* card = shown[i];
        PillItem item;
        item.card = card;
        item.x = x;
        item.glyphW = showIcon ? glyphW : 0;
        item.capW = showCapsule ? capW : 0;
        item.textW = TextWidth(pg, PercentText(*card), g.fonts.pillPct.get());

        float cursor = x;
        if (showIcon) cursor += glyphW + iconGap;
        if (showCapsule) cursor += capW + nubGap + nubW;
        if (card->reading.charging) {
            item.boltW = boltAdvance;
            item.boltX = cursor + boltGap;
            cursor = item.boltX + boltAdvance;
        }
        item.textX = cursor + textGap;
        items.push_back(item);
        x = item.textX + item.textW + margin;
        if (i + 1 < shown.size()) x += margin;
    }

    const bool empty = items.empty();
    float emptyW = 0;
    if (empty) {
        emptyW = TextWidth(pg, L"\u2014", g.fonts.pillPct.get());
        x = padX + 4 * g.scale + glyphW + 5 * g.scale + emptyW + 4 * g.scale + padX;
    }

    const int width = std::max((int)std::lround(x), (int)std::lround(46 * g.scale));
    Canvas canvas(width, height);
    Graphics& g2 = *canvas.g;
    g2.SetPixelOffsetMode(PixelOffsetModeNone);

    // Alpha 1 over the whole strip keeps the window clickable. The optional Fluent taskbar plate
    // and the hover wash are painted on top.
    FillRound(g2, RectF(0, rootY, (float)width, rootH), radius, Color(1, 0, 0, 0));
    if (g.settings->pillBackground) {
        FillRound(g2, RectF(0, rootY, (float)width, rootH), radius, g.theme.pillBg);
        StrokeRound(g2, RectF(0.5f, rootY + 0.5f, width - 1.0f, rootH - 1.0f), radius,
                    g.theme.pillBorder, 1.0f);
    }
    if (g.pillHover) {
        FillRound(g2, RectF(0, rootY, (float)width, rootH), radius, g.theme.pillHover);
        StrokeRound(g2, RectF(0.5f, rootY + 0.5f, width - 1.0f, rootH - 1.0f), radius,
                    g.theme.pillHoverBorder, 1.0f);
    }

    const float cy = rootY + rootH / 2;

    // The pill draws its text with DirectWrite (the WPF rasterizer) when available; the GDI+ path
    // remains as a fallback. Runs are collected and composited after the shapes.
    std::vector<dwtext::Run> textRuns;
    auto addText = [&](const std::wstring& value, Gdiplus::Font* fallback, dwtext::Face face, float px,
                       int weight, RectF box, Color color, StringAlignment align) {
        if (dwtext::Available()) {
            dwtext::Run run;
            run.text = value;
            run.x = box.X;
            run.y = box.Y;
            run.w = box.Width;
            run.h = box.Height;
            run.align = align == StringAlignmentCenter ? 1 : align == StringAlignmentFar ? 2 : 0;
            run.face = face;
            run.px = px;
            run.weight = weight;
            run.argb = ((uint32_t)color.GetA() << 24) | ((uint32_t)color.GetR() << 16) |
                       ((uint32_t)color.GetG() << 8) | (uint32_t)color.GetB();
            textRuns.push_back(std::move(run));
        } else {
            Text(g2, value, fallback, box, color, align);
        }
    };

    if (empty) {
        addText(L"\uE702", g.fonts.pillGlyph.get(), dwtext::Face::Glyph, 13.5f * g.scale, 400,
                RectF(padX + 4 * g.scale, cy - 9 * g.scale, glyphW + 2 * g.scale, 18 * g.scale),
                g.theme.pillSub, StringAlignmentNear);
        addText(L"\u2014", g.fonts.pillPct.get(), dwtext::Face::Ui, 12.0f * g.scale, 600,
                RectF(padX + 4 * g.scale + glyphW + 5 * g.scale, cy - 9 * g.scale, emptyW + 4 * g.scale, 18 * g.scale),
                g.theme.pillSub, StringAlignmentNear);
    }

    for (const auto& item : items) {
        const Card& card = *item.card;
        const bool off = card.reading.asleep || !card.reading.connected;
        const BYTE alpha = card.reading.asleep ? 150 : 255;

        if (showIcon) {
            const Color glyphColor = off ? g.theme.pillSub
                                   : card.reading.charging ? g.theme.pillInk
                                   : card.reading.percent <= 20 ? g.theme.battLow : g.theme.pillInk;
            addText(KindGlyph(card.reading.kind), g.fonts.pillGlyph.get(), dwtext::Face::Glyph,
                    13.5f * g.scale, 400,
                    RectF(item.x, cy - 9 * g.scale, item.glyphW + 2 * g.scale, 18 * g.scale),
                    WithAlpha(glyphColor, alpha), StringAlignmentNear);
        }

        if (showCapsule) {
            const float cx = item.x + item.glyphW + iconGap;
            const float capTop = cy - capH / 2;
            FillRound(g2, RectF(cx, capTop, capW, capH), 2.2f * g.scale, g.theme.battTrack);
            if (card.reading.percent >= 0) {
                const float innerW = capW - 2 * (capBorder + capPad);
                const float fillW = std::clamp(card.reading.percent / 100.0f * innerW,
                                               1.0f * g.scale, innerW);
                FillRound(g2, RectF(cx + capBorder + capPad, capTop + capBorder + capPad,
                                    fillW, capH - 2 * (capBorder + capPad)),
                          1.0f * g.scale, WithAlpha(PillTier(card), alpha));
            }
            StrokeRound(g2, RectF(cx + 0.55f * g.scale, capTop + 0.55f * g.scale,
                                  capW - 1.1f * g.scale, capH - 1.1f * g.scale),
                        2.0f * g.scale, WithAlpha(g.theme.battFrame, alpha), capBorder);
            FillRound(g2, RectF(cx + capW + nubGap, cy - nubH / 2, nubW, nubH),
                      1.0f * g.scale, WithAlpha(g.theme.battFrame, alpha));
        }

        if (item.boltX >= 0) {
            addText(L"\uE945", g.fonts.pillBolt.get(), dwtext::Face::Glyph, 9.5f * g.scale, 400,
                    RectF(item.boltX, cy - 8 * g.scale, item.boltW + 2 * g.scale, 16 * g.scale),
                    WithAlpha(g.theme.battGood, alpha), StringAlignmentNear);
        }

        Color textColor = off ? g.theme.pillSub
                          : card.reading.charging ? g.theme.battGood
                          : card.reading.percent <= 20 ? g.theme.battLow : g.theme.pillInk;
        addText(PercentText(card), g.fonts.pillPct.get(), dwtext::Face::Ui, 12.0f * g.scale, 600,
                RectF(item.textX, cy - 9 * g.scale, item.textW + 4 * g.scale, 18 * g.scale),
                WithAlpha(textColor, alpha), StringAlignmentNear);
    }

    if (!textRuns.empty()) {
        canvas.g->Flush(FlushIntentionSync);
        BitmapData data{};
        Rect rect(0, 0, width, height);
        if (canvas.bitmap->LockBits(&rect, ImageLockModeRead | ImageLockModeWrite,
                                    PixelFormat32bppPARGB, &data) == Ok) {
            dwtext::DrawRuns(data.Scan0, data.Stride, width, height, textRuns);
            canvas.bitmap->UnlockBits(&data);
        }
    }

    int px = 0, py = 0;
    if (g.settings->pillFree) {
        const RECT& mon = layout.monitor;
        px = (int)mon.left + (int)std::lround(g.settings->pillFreeX * g.scale);
        py = (int)mon.top + (int)std::lround(g.settings->pillFreeY * g.scale);
        px = std::clamp(px, (int)mon.left + 4, (int)mon.right - width - 4);
        py = std::clamp(py, (int)mon.top + 4, (int)mon.bottom - height - 4);
        g.pillRect = { px, py, px + width, py + height };
        canvas.Present(g.pill, px, py);
        return;
    }
    if (!layout.found) {
        // A monitor without its own taskbar: sit just above the bottom of its work area.
        px = layout.work.right - width - (int)std::lround(12 * g.scale);
        py = layout.work.bottom - height - (int)std::lround(4 * g.scale);
        px = std::clamp(px, (int)layout.work.left + 4, (int)layout.work.right - width - 4);
        py = std::clamp(py, (int)layout.monitor.top + 4, (int)layout.monitor.bottom - height - 4);
        g.pillRect = { px, py, px + width, py + height };
        canvas.Present(g.pill, px, py);
        return;
    }

    const bool barAtTop = layout.atTop;
    if (g.settings->pillManual) {
        px = barRect.right - (int)std::lround(g.settings->pillOffset * g.scale) - width;
    } else if (g.settings->pillPlacement == 1) {
        px = (notifyRect.right > notifyRect.left ? notifyRect.left : barRect.right - (int)std::lround(160 * g.scale)) -
             width - (int)std::lround(8 * g.scale);
    } else {
        px = (notifyRect.right > notifyRect.left ? notifyRect.right : barRect.right) -
             (int)std::lround(12 * g.scale) - width;
    }
    const int minLeft = barRect.left + (int)std::lround(4 * g.scale);
    const int maxLeft = barRect.right - width - (int)std::lround(4 * g.scale);
    if (maxLeft < minLeft) return;
    px = std::clamp(px, minLeft, maxLeft);

    if (g.settings->pillPlacement == 1) {
        py = barRect.top + (barH - height) / 2;
    } else if (barAtTop) {
        py = barRect.bottom + (int)std::lround(4 * g.scale);
    } else {
        py = barRect.top - height - (int)std::lround(4 * g.scale);
    }

    g.pillRect = { px, py, px + width, py + height };
    canvas.Present(g.pill, px, py);
}

// ------------------------------------------------------------------ drawing: panel

int MeasurePanelHeight(int count) {
    const int pad = (int)std::lround(16 * g.scale);
    const int cardH = (int)std::lround(80 * g.scale);
    const int gap = (int)std::lround(10 * g.scale);
    const int body = count > 0 ? count * cardH + (count - 1) * gap : (int)std::lround(84 * g.scale);
    return pad + body + pad;
}

float TargetPercent(const Card& card) {
    const int pct = card.reading.percent;
    return pct < 0 ? 0.0f : (float)std::clamp(pct, 0, 100);
}

void DrawBatteryLevel(Graphics& g2, RectF box, const Card& card, float shownPercent, bool hidden) {
    if (hidden) {
        const float level = std::clamp(shownPercent, 0.0f, 100.0f) / 10.0f;
        const int lo = std::clamp((int)std::floor(level), 0, 10);
        const int hi = std::min(10, lo + 1);
        const float frac = level - (float)lo;
        g.icons.Draw(g2, icoBattery0 + lo, box, g.theme.faint, 130);
        if (frac > 0.02f) g.icons.Draw(g2, icoBattery0 + hi, box, g.theme.faint, (BYTE)(130 * frac));
        return;
    }
    if (card.reading.charging) {
        // Ready-made Fluent "battery charge" mark, breathing while on power.
        const float pulse = 0.5f + 0.5f * sinf(g.spin * 0.12f);
        g.icons.Draw(g2, icoBatteryCharge, box, g.theme.green, (BYTE)(150 + 90 * pulse));
        return;
    }
    const float level = std::clamp(shownPercent, 0.0f, 100.0f) / 10.0f;
    const int lo = std::clamp((int)std::floor(level), 0, 10);
    const int hi = std::min(10, lo + 1);
    const float frac = level - (float)lo;
    const Color tint = IsLow(card) ? g.theme.red : g.theme.text;
    const BYTE alpha = card.reading.asleep ? 150 : 255;
    g.icons.Draw(g2, icoBattery0 + lo, box, tint, alpha);
    if (frac > 0.02f) g.icons.Draw(g2, icoBattery0 + hi, box, tint, (BYTE)(alpha * frac));
}

void DrawCard(Graphics& g2, const Card& card, RectF box) {
    const bool hidden = HiddenFromPill(card.reading.id);
    const int cardIndex = (int)(&card - g.cards.data());
    const bool hovered = g.hoveredCard >= 0 && (size_t)g.hoveredCard < g.cards.size() &&
                         &card == &g.cards[(size_t)g.hoveredCard];
    FillRound(g2, box, 16 * g.scale,
              hidden ? WithAlpha(g.theme.card, hovered ? 170 : 150)
                     : (hovered ? g.theme.cardHover : g.theme.card));

    const float glyphX = box.X + 16 * g.scale;
    const float glyphSize = 22 * g.scale;
    Color glyphColor = hidden ? g.theme.faint
                     : card.reading.charging ? g.theme.green
                     : IsLow(card) ? g.theme.red : g.theme.text;
    if (card.reading.asleep || !card.reading.connected) glyphColor = g.theme.faint;
    g.icons.Draw(g2, IconFor(card.reading.kind), RectF(glyphX, box.Y + 16 * g.scale, glyphSize, glyphSize),
                 glyphColor, hidden ? 130 : (card.reading.asleep ? 150 : 255));

    const float right = box.GetRight() - 16 * g.scale;
    const float battW = 24 * g.scale;
    const float pctW = 72 * g.scale;
    const float textX = glyphX + glyphSize + 12 * g.scale;
    const float textW = right - battW - 8 * g.scale - pctW - 10 * g.scale - textX;
    const BYTE textAlpha = hidden ? 110 : (card.reading.asleep || !card.reading.connected ? 165 : 255);

    Text(g2, card.reading.name, g.fonts.name.get(), RectF(textX, box.Y + 10 * g.scale, textW, 22 * g.scale),
         hidden ? WithAlpha(g.theme.faint, textAlpha) : WithAlpha(g.theme.text, textAlpha),
         StringAlignmentNear, true);

    Color statusColor = hidden ? g.theme.faint : g.theme.sub;
    if (!hidden) {
        if (card.reading.charging) statusColor = g.theme.green;
        else if (IsLow(card)) statusColor = g.theme.red;
        else if (card.forecast.state == ForecastState::Ready || card.forecast.state == ForecastState::LowConfidence)
            statusColor = g.theme.accent;
    }
    Text(g2, StatusLine(card), g.fonts.status.get(), RectF(textX, box.Y + 33 * g.scale, textW, 20 * g.scale),
         WithAlpha(statusColor, textAlpha), StringAlignmentNear, true);

    // Ready-made battery icons (Fluent): the level morphs between two neighbouring icons, and the
    // number counts up, so a level change reads as motion instead of a jump.
    const float shown = g.ringValues.count(card.reading.id) ? g.ringValues[card.reading.id]
                                                           : TargetPercent(card);
    const RectF batt(right - battW, box.Y + 11 * g.scale, battW, battW);
    DrawBatteryLevel(g2, batt, card, shown, hidden);
    const Color pctColor = hidden ? g.theme.faint : PercentColor(card);
    Text(g2, Fmt(L"%d%%", (int)std::lround(shown)), g.fonts.percent.get(),
         RectF(right - battW - 8 * g.scale - pctW, box.Y + 8 * g.scale, pctW, 28 * g.scale),
         WithAlpha(pctColor, textAlpha), StringAlignmentFar);

    // Eye: show/hide this device on the taskbar pill. The panel keeps the row (desaturated), so it
    // can always be brought back.
    const RectF eye(box.GetRight() - 16 * g.scale - 26 * g.scale,
                    box.GetBottom() - 6 * g.scale - 26 * g.scale, 26 * g.scale, 26 * g.scale);
    g.eyeRects.push_back(eye);
    const bool eyeHover = g.hoveredEye == cardIndex;
    if (eyeHover) FillRound(g2, eye, 7 * g.scale, WithAlpha(g.theme.text, 14));
    const Color eyeColor = hidden ? g.theme.faint : (eyeHover ? g.theme.text : g.theme.sub);
    const float ex = eye.X + eye.Width / 2, ey = eye.Y + eye.Height / 2;
    Pen eyePen(eyeColor, 1.5f * g.scale);
    g2.DrawEllipse(&eyePen, ex - 9 * g.scale, ey - 4.5f * g.scale, 18 * g.scale, 9 * g.scale);
    SolidBrush pupil(eyeColor);
    g2.FillEllipse(&pupil, ex - 2.8f * g.scale, ey - 2.8f * g.scale, 5.6f * g.scale, 5.6f * g.scale);
    if (hidden) g2.DrawLine(&eyePen, ex - 10 * g.scale, ey + 6 * g.scale, ex + 10 * g.scale, ey - 6 * g.scale);
}

void PlaceAndPresentPanel(Canvas& canvas, int width, int height) {
    const BarLayout layout = BarForMonitor(MonitorAt(g.settings->monitorIndex));
    const RECT bar = layout.bar;
    RECT work = layout.work;
    if (work.right <= work.left || work.bottom <= work.top) {
        work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }
    const bool barAtTop = layout.found && layout.atTop;
    const int anchorLeft = g.pillRect.right > g.pillRect.left ? g.pillRect.left
                                                               : (layout.found ? bar.right - 100 : work.right - 100);
    const int anchorRight = g.pillRect.right > g.pillRect.left ? g.pillRect.right
                                                                : (layout.found ? bar.right : work.right);
    const int center = (anchorLeft + anchorRight) / 2;
    int panelX = center - width / 2;
    int panelY = 0;
    bool belowTaskbar = false;
    if (g.settings->pillFree) {
        const int above = g.pillRect.top - height - (int)std::lround(8 * g.scale);
        panelY = above >= (int)work.top + 8 ? above
                                            : g.pillRect.bottom + (int)std::lround(8 * g.scale);
    } else if (barAtTop) {
        belowTaskbar = true;
        panelY = work.top + (int)std::lround(8 * g.scale);
    } else if (g.pillRect.top > 0 && (!layout.found || g.pillRect.top < bar.top)) {
        panelY = g.pillRect.top - height - (int)std::lround(8 * g.scale);
    } else {
        panelY = work.bottom - height - (int)std::lround(8 * g.scale);
    }
    panelX = std::clamp(panelX, (int)work.left + 8, (int)work.right - width - 8);
    panelY = std::clamp(panelY, (int)work.top + 8, (int)work.bottom - height - 8);

    // A short slide (about 10 DIP) plus the fade, like the shell's own flyouts.
    const int slide = (int)std::lround((1.0f - std::clamp(g.reveal, 0.0f, 1.0f)) * 10 * g.scale);
    const int visualY = panelY + (belowTaskbar ? -slide : slide);
    g.panelPos = { panelX, visualY };
    canvas.Present(g.panel, panelX, visualY, (BYTE)std::lround(255 * std::clamp(g.reveal, 0.0f, 1.0f)));
}

void RenderSettings();

void RenderPanel() {
    if (!g.panel) return;
    if (g.panelMode == PanelMode::Settings) {
        RenderSettings();
        return;
    }
    const int pad = (int)std::lround(16 * g.scale);
    const int width = (int)std::lround(440 * g.scale);
    const int count = (int)g.cards.size();
    const int height = MeasurePanelHeight(count);
    const int cardH = (int)std::lround(80 * g.scale);
    const int cardGap = (int)std::lround(10 * g.scale);
    const int radius = (int)std::lround(20 * g.scale);

    Canvas canvas(width, height);
    Graphics& g2 = *canvas.g;

    FillRound(g2, RectF(0, 0, (float)width, (float)height), (float)radius, g.theme.bg);
    StrokeRound(g2, RectF(0.5f, 0.5f, width - 1.0f, height - 1.0f), (float)radius, g.theme.border, 1.0f);

    // ---- cards
    g.cardRects.clear();
    g.eyeRects.clear();
    float y = (float)pad;
    if (count == 0) {
        RectF box((float)pad, y, (float)(width - pad * 2), 84 * g.scale);
        FillRound(g2, box, 16 * g.scale, g.theme.card);
        Text(g2, Tr(L"Нет устройств с батареей", L"No battery devices"), g.fonts.status.get(),
             RectF(box.X, box.Y + 20 * g.scale, box.Width, 20 * g.scale), g.theme.sub, StringAlignmentCenter);
        Text(g2, Tr(L"Включите мышь, наушники или подключите донгл", L"Turn on your mouse, headset or receiver"),
             g.fonts.small.get(), RectF(box.X + 10 * g.scale, box.Y + 44 * g.scale, box.Width - 20 * g.scale, 18 * g.scale),
             g.theme.faint, StringAlignmentCenter, true);
    } else {
        for (int i = 0; i < count; i++) {
            RectF box((float)pad, y, (float)(width - pad * 2), (float)cardH);
            g.cardRects.push_back(box);
            DrawCard(g2, g.cards[(size_t)i], box);
            y += cardH + cardGap;
        }
    }

    // ---- anchor above the widget / taskbar
    PlaceAndPresentPanel(canvas, width, height);
}

// ------------------------------------------------------------------ drawing: settings panel

void DrawToggle(Graphics& g2, RectF track, bool on, bool hovered) {
    const float radius = track.Height / 2;
    const Color off = WithAlpha(g.theme.text, hovered ? 54 : 34);
    FillRound(g2, track, radius, on ? g.theme.accent : off);
    const float knob = track.Height - 6 * g.scale;
    const float x = on ? track.GetRight() - 3 * g.scale - knob : track.X + 3 * g.scale;
    SolidBrush brush(Color(255, 255, 255, 255));
    g2.FillEllipse(&brush, x, track.Y + 3 * g.scale, knob, knob);
}

void DrawSegments(Graphics& g2, RectF box, const std::vector<std::wstring>& labels, int selected, int id) {
    FillRound(g2, box, 8 * g.scale, WithAlpha(g.theme.text, 16));
    const float segW = box.Width / std::max<size_t>(1, labels.size());
    for (size_t i = 0; i < labels.size(); i++) {
        RectF inner(box.X + segW * (float)i + 2 * g.scale, box.Y + 2 * g.scale,
                    segW - 4 * g.scale, box.Height - 4 * g.scale);
        const size_t hitIndex = g.settingHits.size();
        g.settingHits.push_back({ id, (int)i, RectF(box.X + segW * (float)i, box.Y, segW, box.Height) });
        const bool hovered = g.hoveredSetting == (int)hitIndex;
        if ((int)i == selected)
            FillRound(g2, inner, 6 * g.scale, g.theme.accent);
        else if (hovered)
            FillRound(g2, inner, 6 * g.scale, WithAlpha(g.theme.text, 12));
        Text(g2, labels[i], g.fonts.small.get(), inner,
             (int)i == selected ? Color(255, 255, 255, 255) : g.theme.sub, StringAlignmentCenter);
    }
}

std::vector<std::wstring> MonitorLabels() {
    std::vector<std::wstring> labels;
    const auto monitors = Monitors();
    int extra = 0;
    for (size_t i = 0; i < monitors.size() && i < 3; i++) {
        if (monitors[i].primary) {
            labels.push_back(Tr(L"Основной", L"Primary"));
        } else {
            ++extra;
            labels.push_back(extra == 1 ? Tr(L"Дополнительный", L"Additional")
                                        : Fmt(L"%s %d", Tr(L"Дополнительный", L"Additional"), extra));
        }
    }
    return labels;
}

void ApplySettingHit(const SettingHit& hit) {
    switch (hit.id) {
        case kSetRefresh:
            if (g.hooks.refresh) g.hooks.refresh();
            return;
        case kSetExit:
            if (g.hooks.exit) g.hooks.exit();
            return;
        case kSetAutostart: g.settings->autoStart = !g.settings->autoStart; break;
        case kSetMove: g.settings->pillUnlocked = !g.settings->pillUnlocked; break;
        case kSetBackground: g.settings->pillBackground = !g.settings->pillBackground; break;
        case kSetFullscreen: g.settings->hideFullscreen = !g.settings->hideFullscreen; break;
        case kSetPlacement:
            g.settings->pillPlacement = std::clamp(hit.option, 0, 1);
            g.settings->pillFree = false;
            break;
        case kSetStyle: g.settings->pillMode = std::clamp(hit.option, 0, 2); break;
        case kSetMonitor: g.settings->monitorIndex = std::clamp(hit.option, 0, 15); break;
        case kSetLanguage: g.settings->language = std::clamp(hit.option, 0, 2); break;
        case kSetReset:
            g.settings->pillFree = false;
            g.settings->pillManual = false;
            g.settings->pillOffset = 12;
            break;
        default: return;
    }
    if (g.hooks.settingsApplied) g.hooks.settingsApplied();
    RenderPanel();
}

void RenderSettings() {
    if (!g.panel) return;
    const int pad = (int)std::lround(16 * g.scale);
    const int width = (int)std::lround(440 * g.scale);
    const int headerH = (int)std::lround(48 * g.scale);
    const int cardX = pad;
    const int cardY = pad + headerH + (int)std::lround(4 * g.scale);
    const int cardW = width - pad * 2;
    const int radius = (int)std::lround(20 * g.scale);

    const auto monitors = Monitors();
    std::vector<int> rows = { kSetAutostart, kSetMove, kSetBackground, kSetFullscreen,
                              kSetPlacement, kSetStyle };
    if (monitors.size() > 1) rows.push_back(kSetMonitor);
    rows.push_back(kSetLanguage);
    rows.push_back(kSetReset);

    const int rowH = (int)std::lround(46 * g.scale);
    const int cardH = (int)rows.size() * rowH + (int)(rows.size() - 1);
    const int buttonH = (int)std::lround(38 * g.scale);
    const int buttonGap = (int)std::lround(10 * g.scale);
    const int buttonsY = cardY + cardH + (int)std::lround(12 * g.scale);
    const int height = buttonsY + buttonH + pad;

    Canvas canvas(width, height);
    Graphics& g2 = *canvas.g;
    FillRound(g2, RectF(0, 0, (float)width, (float)height), (float)radius, g.theme.bg);
    StrokeRound(g2, RectF(0.5f, 0.5f, width - 1.0f, height - 1.0f), (float)radius, g.theme.border, 1.0f);

    Text(g2, L"WattPill", g.fonts.wordmark.get(),
         RectF((float)pad, (float)pad - 2 * g.scale, 240 * g.scale, 32 * g.scale),
         g.theme.text, StringAlignmentNear);
    Text(g2, Tr(L"Настройки", L"Settings"), g.fonts.status.get(),
         RectF((float)(width - pad) - 180 * g.scale, (float)pad + 2 * g.scale, 180 * g.scale, 26 * g.scale),
         g.theme.sub, StringAlignmentFar);

    FillRound(g2, RectF((float)cardX, (float)cardY, (float)cardW, (float)cardH), 16 * g.scale, g.theme.card);
    g.settingHits.clear();

    const auto labels = MonitorLabels();
    float y = (float)cardY;
    for (size_t r = 0; r < rows.size(); r++) {
        const int id = rows[r];
        const RectF row((float)cardX, y, (float)cardW, (float)rowH);
        const float labelX = row.X + 16 * g.scale;
        const float right = row.GetRight() - 16 * g.scale;
        const std::wstring label =
            id == kSetAutostart ? Tr(L"Автозагрузка", L"Start with Windows")
            : id == kSetMove ? Tr(L"Перемещать виджет", L"Move widget")
            : id == kSetBackground ? Tr(L"Фон пилюли", L"Pill background")
            : id == kSetFullscreen ? Tr(L"Скрывать в полноэкранном режиме", L"Hide in fullscreen")
            : id == kSetPlacement ? Tr(L"Расположение", L"Placement")
            : id == kSetStyle ? Tr(L"Вид пилюли", L"Pill style")
            : id == kSetMonitor ? Tr(L"Монитор", L"Monitor")
            : id == kSetLanguage ? Tr(L"Язык", L"Language")
            : Tr(L"Сбросить позицию", L"Reset position");

        const bool isReset = id == kSetReset;
        float reserve = 0;
        if (id == kSetAutostart || id == kSetMove || id == kSetBackground || id == kSetFullscreen)
            reserve = 64 * g.scale;
        else if (id == kSetPlacement) reserve = 172 * g.scale;
        else if (id == kSetStyle) reserve = 222 * g.scale;
        else if (id == kSetMonitor) reserve = 240 * g.scale;
        else if (id == kSetLanguage) reserve = 172 * g.scale;
        const float labelW = std::max(40 * g.scale, right - labelX - reserve - 6 * g.scale);
        Text(g2, label, g.fonts.status.get(),
             RectF(labelX, row.Y, labelW, row.Height),
             isReset ? g.theme.accent : g.theme.text, StringAlignmentNear, true);

        if (id == kSetAutostart || id == kSetMove || id == kSetBackground || id == kSetFullscreen) {
            const float trackW = 40 * g.scale, trackH = 22 * g.scale;
            const RectF track(right - trackW, row.Y + (row.Height - trackH) / 2, trackW, trackH);
            const bool on = id == kSetAutostart ? g.settings->autoStart
                          : id == kSetMove ? g.settings->pillUnlocked
                          : id == kSetBackground ? g.settings->pillBackground
                                                 : g.settings->hideFullscreen;
            const size_t hitIndex = g.settingHits.size();
            g.settingHits.push_back({ id, -1, row });
            DrawToggle(g2, track, on, g.hoveredSetting == (int)hitIndex);
        } else if (id == kSetPlacement) {
            const std::vector<std::wstring> seg = { Tr(L"Над часами", L"Above clock"),
                                                    Tr(L"На панели", L"Taskbar") };
            const float segW = 160 * g.scale, segH = 28 * g.scale;
            DrawSegments(g2, RectF(right - segW, row.Y + (row.Height - segH) / 2, segW, segH), seg,
                         g.settings->pillPlacement, id);
        } else if (id == kSetStyle) {
            const std::vector<std::wstring> seg = { Tr(L"Полный", L"Full"),
                                                    Tr(L"Компакт", L"Compact"),
                                                    Tr(L"Минимум", L"Minimal") };
            const float segW = 210 * g.scale, segH = 28 * g.scale;
            DrawSegments(g2, RectF(right - segW, row.Y + (row.Height - segH) / 2, segW, segH), seg,
                         g.settings->pillMode, id);
        } else if (id == kSetMonitor) {
            const float segW = 230 * g.scale, segH = 28 * g.scale;
            DrawSegments(g2, RectF(right - segW, row.Y + (row.Height - segH) / 2, segW, segH), labels,
                         std::clamp(g.settings->monitorIndex, 0, (int)labels.size() - 1), id);
        } else if (id == kSetLanguage) {
            const std::vector<std::wstring> seg = { Tr(L"Авто", L"Auto"), L"Рус", L"Eng" };
            const float segW = 160 * g.scale, segH = 28 * g.scale;
            DrawSegments(g2, RectF(right - segW, row.Y + (row.Height - segH) / 2, segW, segH), seg,
                         g.settings->language, id);
        } else if (isReset) {
            const size_t hitIndex = g.settingHits.size();
            g.settingHits.push_back({ id, -1, row });
            if (g.hoveredSetting == (int)hitIndex)
                FillRound(g2, RectF(row.X + 4 * g.scale, row.Y + 3 * g.scale,
                                    row.Width - 8 * g.scale, row.Height - 6 * g.scale),
                          10 * g.scale, WithAlpha(g.theme.text, 12));
        }

        y += rowH;
        if (r + 1 < rows.size()) {
            const float lineY = y;
            SolidBrush line(WithAlpha(g.theme.text, 14));
            g2.FillRectangle(&line, RectF(row.X + 16 * g.scale, lineY, row.Width - 32 * g.scale, 1.0f));
        }
    }

    const float buttonW = (cardW - buttonGap) / 2.0f;
    const RectF refreshBtn((float)cardX, (float)buttonsY, buttonW, (float)buttonH);
    const RectF exitBtn((float)(cardX + buttonW + buttonGap), (float)buttonsY, buttonW, (float)buttonH);
    for (int b = 0; b < 2; b++) {
        const RectF rect = b == 0 ? refreshBtn : exitBtn;
        const Color color = b == 0 ? g.theme.accent : g.theme.red;
        const size_t hitIndex = g.settingHits.size();
        g.settingHits.push_back({ b == 0 ? kSetRefresh : kSetExit, -1, rect });
        const bool hovered = g.hoveredSetting == (int)hitIndex;
        FillRound(g2, rect, 10 * g.scale, g.theme.card);
        if (hovered)
            FillRound(g2, RectF(rect.X + 1, rect.Y + 1, rect.Width - 2, rect.Height - 2),
                      9 * g.scale, WithAlpha(color, 22));
        StrokeRound(g2, RectF(rect.X + 0.5f, rect.Y + 0.5f, rect.Width - 1.0f, rect.Height - 1.0f),
                    10 * g.scale, WithAlpha(color, hovered ? 110 : 55), 1.0f);
        Text(g2, b == 0 ? Tr(L"Обновить", L"Refresh") : Tr(L"Выход", L"Exit"),
             g.fonts.status.get(), rect, color, StringAlignmentCenter);
    }

    PlaceAndPresentPanel(canvas, width, height);
}

// ------------------------------------------------------------------ theme & windows

void BuildTheme() {
    const bool light = AppIsLight();
    if (light) {
        g.theme.bg = Color(242, 249, 249, 249);
        g.theme.card = Color(255, 255, 255, 255);
        g.theme.cardHover = Color(255, 239, 239, 239);
        g.theme.border = Color(30, 60, 60, 67);
        g.theme.text = Color(255, 17, 17, 19);
        g.theme.sub = Color(255, 108, 108, 116);
        g.theme.faint = Color(255, 142, 142, 150);
        g.theme.red = Color(255, 215, 0, 21);
        g.theme.green = Color(255, 36, 138, 61);
        g.theme.amber = Color(255, 157, 93, 0);
        g.theme.charging = Color(255, 0, 95, 184);
    } else {
        g.theme.bg = Color(236, 32, 32, 32);
        g.theme.card = Color(255, 43, 43, 43);
        g.theme.cardHover = Color(255, 51, 51, 51);
        g.theme.border = Color(26, 255, 255, 255);
        g.theme.text = Color(255, 255, 255, 255);
        g.theme.sub = Color(255, 161, 161, 166);
        g.theme.faint = Color(255, 124, 124, 130);
        g.theme.red = Color(255, 255, 69, 58);
        g.theme.green = Color(255, 48, 209, 88);
        g.theme.amber = Color(255, 252, 225, 0);
        g.theme.charging = Color(255, 96, 205, 255);
    }

    const bool taskbarLight = TaskbarIsLight();
    const COLORREF accent = SystemAccentColor(taskbarLight);
    if (taskbarLight) {
        g.theme.pillBg = Color(133, 255, 255, 255);
        g.theme.pillBorder = Color(30, 0, 0, 0);
        g.theme.pillHover = Color(20, 0, 0, 0);
        g.theme.pillHoverBorder = Color(28, 0, 0, 0);
        g.theme.pillInk = Color(255, 26, 26, 26);
        g.theme.pillSub = Color(255, 90, 90, 90);
        g.theme.battFrame = Color(255, 107, 107, 107);
        g.theme.battTrack = Color(26, 0, 0, 0);
        g.theme.battGood = Color(255, 15, 123, 15);
        g.theme.battMid = Color(255, 157, 93, 0);
        g.theme.battLow = Color(255, 196, 43, 28);
        g.theme.battCharging = Color(255, GetRValue(accent), GetGValue(accent), GetBValue(accent));
    } else {
        g.theme.pillBg = Color(102, 44, 44, 44);
        g.theme.pillBorder = Color(34, 255, 255, 255);
        g.theme.pillHover = Color(22, 255, 255, 255);
        g.theme.pillHoverBorder = Color(34, 255, 255, 255);
        g.theme.pillInk = Color(255, 255, 255, 255);
        g.theme.pillSub = Color(255, 200, 200, 200);
        g.theme.battFrame = Color(255, 184, 184, 184);
        g.theme.battTrack = Color(36, 255, 255, 255);
        g.theme.battGood = Color(255, 108, 203, 95);
        g.theme.battMid = Color(255, 252, 225, 0);
        g.theme.battLow = Color(255, 255, 153, 164);
        g.theme.battCharging = Color(255, 96, 205, 255);
    }
}

UINT WindowDpi(HWND wnd) {
    using GetDpiFn = UINT(WINAPI*)(HWND);
    static GetDpiFn getDpi = nullptr;
    static bool resolved = false;
    if (!resolved) {
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
            getDpi = (GetDpiFn)GetProcAddress(user32, "GetDpiForWindow");
        resolved = true;
    }
    UINT dpi = (getDpi && wnd) ? getDpi(wnd) : 0;
    if (!dpi) {
        HDC dc = GetDC(nullptr);
        dpi = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(nullptr, dc);
    }
    return dpi ? dpi : 96;
}

void RebuildFonts() {
    const UINT dpi = WindowDpi(g.pill ? g.pill : g.panel);
    g.scale = std::max(1.0f, dpi / 96.0f);
    g.fonts.Build(g.scale, g.uiFontVariable, g.familyRegular, g.familyMedium, g.familySemibold);
}

void ShowPanelInternal(PanelMode mode) {
    g.panelMode = mode;
    g.visible = true;
    if (!g.panel) return;
    HideTip();
    if (!g.reveal) g.reveal = 0.0f;
    g.animLast = 0;
    g.hoveredCard = -1;
    g.hoveredEye = -1;
    g.hoveredSetting = -1;
    RenderPanel();
    ShowWindow(g.panel, SW_SHOWNOACTIVATE);
    SetWindowPos(g.panel, HWND_TOPMOST, g.panelPos.x, g.panelPos.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(g.panel, kPanelTimerAnim, 33, nullptr);
}

void HidePanelInternal() {

    g.visible = false;
    g.hoveredCard = -1;
    g.hoveredEye = -1;
    g.hoveredSetting = -1;
    if (!g.panel) return;
    KillTimer(g.panel, kPanelTimerHide);
    KillTimer(g.panel, kPanelTimerAnim);
    ShowWindow(g.panel, SW_HIDE);
}

bool CursorInside(HWND wnd, const RECT& rect) {
    POINT p{};
    if (!GetCursorPos(&p)) return false;
    return p.x >= rect.left && p.x < rect.right && p.y >= rect.top && p.y < rect.bottom;
}

void TrackLeave(HWND wnd) {
    TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, wnd, 0 };
    TrackMouseEvent(&t);
}

// Foreground changes are the main reason the taskbar rises above our topmost pill. React at once
// instead of waiting for the next guard tick.
void CALLBACK OnForegroundEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (g.pill) PostMessageW(g.pill, WM_APP_REFRESH_UI, 0, 0);
}

// Borderless ("fullscreen windowed") games cover the whole monitor, including the strip the
// taskbar lives on, but Windows still reports a normal notification state. Detect the window
// itself: a borderless game covers the monitor with its client area, while a normal maximized
// document has a caption and its client starts below the title bar, so a covering window rect
// alone is not enough.
// PowerVision's rule, reproduced exactly: the foreground root window must be on the same monitor
// as the widget, be visible and uncloaked, and its window rectangle must cover that monitor.
bool ForegroundCoversMonitor(HMONITOR target) {
    if (!target) return false;
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    HWND root = GetAncestor(foreground, GA_ROOT);
    if (!root) root = foreground;

    DWORD pid = 0;
    GetWindowThreadProcessId(root, &pid);
    if (pid == GetCurrentProcessId()) return false;   // our own pill/panel
    if (!IsWindowVisible(root) || IsIconic(root)) return false;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return false;

    wchar_t cls[128] = {};
    GetClassNameW(root, cls, 128);
    if (!_wcsicmp(cls, L"Shell_TrayWnd") || !_wcsicmp(cls, L"Shell_SecondaryTrayWnd") ||
        !_wcsicmp(cls, L"Progman") || !_wcsicmp(cls, L"WorkerW") ||
        !_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") || !_wcsicmp(cls, L"ForegroundStaging"))
        return false;   // the shell itself is never a fullscreen app

    if (MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST) != target) return false;

    RECT window{};
    if (!GetWindowRect(root, &window)) return false;
    MONITORINFO info{ sizeof(info) };
    if (!GetMonitorInfoW(target, &info)) return false;
    const RECT& screen = info.rcMonitor;
    return window.left <= screen.left + 1 && window.top <= screen.top + 1 &&
           window.right >= screen.right - 1 && window.bottom >= screen.bottom - 1;
}

bool FullscreenApp() {
    if (!g.settings) return false;
    return ForegroundCoversMonitor(MonitorAt(g.settings->monitorIndex));
}

std::wstring FullscreenReportImpl() {
    QUERY_USER_NOTIFICATION_STATE state{};
    const HRESULT hr = SHQueryUserNotificationState(&state);
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOT) : nullptr;

    std::wstring out = Fmt(L"QUNS=%d (hr=0x%08X)\n", (int)state, (unsigned)hr);
    if (!root) {
        out += L"no foreground window\n";
        out += Fmt(L"covers=0 fullscreen=%d\n", FullscreenApp() ? 1 : 0);
        return out;
    }

    wchar_t cls[128] = {};
    GetClassNameW(root, cls, 128);
    DWORD pid = 0;
    GetWindowThreadProcessId(root, &pid);
    DWORD cloaked = 0;
    DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    RECT wr{}, cr{}, frame{};
    POINT origin{};
    GetWindowRect(root, &wr);
    GetClientRect(root, &cr);
    ClientToScreen(root, &origin);
    DwmGetWindowAttribute(root, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame));

    HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT barRect{}, monRect{};
    if (bar) {
        GetWindowRect(bar, &barRect);
        HMONITOR mon = MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(mon, &mi)) monRect = mi.rcMonitor;
    }

    out += Fmt(L"class=%s pid=%lu style=0x%08lX ex=0x%08lX zoomed=%d visible=%d iconic=%d cloaked=%lu\n",
               cls, (unsigned long)pid, (unsigned long)GetWindowLongW(root, GWL_STYLE),
               (unsigned long)GetWindowLongW(root, GWL_EXSTYLE), IsZoomed(root) ? 1 : 0,
               IsWindowVisible(root) ? 1 : 0, IsIconic(root) ? 1 : 0, (unsigned long)cloaked);
    out += Fmt(L"window=%ld,%ld,%ld,%ld client=%ld,%ld,%ld,%ld frame=%ld,%ld,%ld,%ld\n",
               wr.left, wr.top, wr.right, wr.bottom,
               origin.x, origin.y, origin.x + cr.right, origin.y + cr.bottom,
               frame.left, frame.top, frame.right, frame.bottom);
    out += Fmt(L"monitor=%ld,%ld,%ld,%ld taskbar=%ld,%ld,%ld,%ld\n",
               monRect.left, monRect.top, monRect.right, monRect.bottom,
               barRect.left, barRect.top, barRect.right, barRect.bottom);
    out += Fmt(L"covers=%d fullscreen=%d\n",
               ForegroundCoversMonitor(MonitorAt(g.settings->monitorIndex)) ? 1 : 0,
               FullscreenApp() ? 1 : 0);
    return out;
}

void Animate() {
    ULONGLONG now = GetTickCount64();
    if (!g.animLast) g.animLast = now;
    const float dt = std::min(0.1f, (float)(now - g.animLast) / 1000.0f);
    g.animLast = now;

    bool active = false;
    for (const auto& card : g.cards) {
        const float target = TargetPercent(card);
        auto it = g.ringValues.find(card.reading.id);
        if (it == g.ringValues.end()) {
            g.ringValues[card.reading.id] = target;   // first paint: no sweep from zero
            continue;
        }
        const float diff = target - it->second;
        if (std::fabs(diff) > 0.08f) {
            it->second += diff * std::min(1.0f, dt * 9.0f);
            active = true;
        } else {
            it->second = target;
        }
        if (card.reading.charging) active = true;
    }

    const bool charging = std::any_of(g.cards.begin(), g.cards.end(),
                                      [](const Card& c) { return c.reading.charging; });
    if (charging) {
        g.spin += dt * 150.0f;
        if (g.spin >= 360.0f) g.spin -= 360.0f;
    }

    if (g.reveal < 1.0f) {
        g.reveal = std::min(1.0f, g.reveal + dt * 8.0f);
        active = true;
    }

    if (active) {
        RenderPanel();
    } else if (g.panel) {
        KillTimer(g.panel, kPanelTimerAnim);
    }
}

// ------------------------------------------------------------------ window procs

LRESULT CALLBACK PillProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            if (g.dragArmed && (GetKeyState(VK_LBUTTON) & 0x8000)) {
                POINT p{};
                GetCursorPos(&p);
                const int dx = p.x - g.dragStartX;
                const int dy = p.y - g.dragStartY;
                if (!g.dragMoved &&
                    std::max(std::abs(dx), std::abs(dy)) >= GetSystemMetrics(SM_CXDRAG))
                    g.dragMoved = true;
                if (g.dragMoved) {
                    const BarLayout layout = BarForMonitor(MonitorAt(g.settings->monitorIndex));
                    if (g.dragFree) {
                        const int width = g.pillRect.right - g.pillRect.left;
                        const int height = g.pillRect.bottom - g.pillRect.top;
                        const RECT& mon = layout.monitor;
                        const int px = std::clamp(g.dragStartLeft + dx, (int)mon.left + 4,
                                                  (int)mon.right - width - 4);
                        const int py = std::clamp(g.dragStartTop + dy, (int)mon.top + 4,
                                                  (int)mon.bottom - height - 4);
                        g.settings->pillFree = true;
                        g.settings->pillFreeX =
                            (int)std::lround((px - (int)mon.left) / g.scale);
                        g.settings->pillFreeY =
                            (int)std::lround((py - (int)mon.top) / g.scale);
                        g.settings->pillManual = false;
                        RenderPill();
                    } else {
                        RECT bar = layout.found ? layout.bar : layout.work;
                        if (g.pillRect.right > g.pillRect.left) {
                            const int width = g.pillRect.right - g.pillRect.left;
                            const int minLeft = bar.left + (int)std::lround(4 * g.scale);
                            const int maxLeft = bar.right - width - (int)std::lround(4 * g.scale);
                            const int left = std::clamp(g.dragStartLeft + dx, minLeft, maxLeft);
                            g.settings->pillManual = true;
                            g.settings->pillOffset =
                                std::clamp((int)std::lround((bar.right - (left + width)) / g.scale), 0, 4000);
                            RenderPill();
                        }
                    }
                }
                return 0;
            }
            if (!g.pillHover) {
                g.pillHover = true;
                TrackLeave(h);
                RenderPill();
                SetTimer(h, kPillTipTimer, 600, nullptr);
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == kPillTipTimer) {
                KillTimer(h, kPillTipTimer);
                ShowTip();
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
            KillTimer(h, kPillTipTimer);
            HideTip();
            {
                const bool shift = GetKeyState(VK_SHIFT) < 0;
                const bool ctrl = GetKeyState(VK_CONTROL) < 0;
                if ((shift || ctrl || g.settings->pillUnlocked) && g.pillRect.right > g.pillRect.left) {
                    POINT p{};
                    GetCursorPos(&p);
                    g.dragArmed = true;
                    g.dragMoved = false;
                    // Ctrl always moves freely; Move widget does the same without a modifier, while
                    // Shift keeps the classic movement along the taskbar.
                    g.dragFree = ctrl || (g.settings->pillUnlocked && !shift);
                    if (!g.dragFree) g.settings->pillFree = false;
                    g.dragStartX = p.x;
                    g.dragStartY = p.y;
                    g.dragStartLeft = g.pillRect.left;
                    g.dragStartTop = g.pillRect.top;
                    SetCapture(h);
                    return 0;
                }
            }
            break;
        case WM_LBUTTONUP: {
            if (g.dragArmed) {
                const bool moved = g.dragMoved;
                g.dragArmed = false;
                g.dragMoved = false;
                if (GetCapture() == h) ReleaseCapture();
                if (moved) {
                    if (g.hooks.settingsChanged) g.hooks.settingsChanged();
                    return 0;
                }
                // A click without movement is still a click: open/close the panel below.
            }
            HideTip();
            KillTimer(h, kPillTipTimer);
            // Debounce: a burst of clicks is one gesture, not a flicker of open/close animations.
            static ULONGLONG lastToggle = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - lastToggle < 300) return 0;
            lastToggle = now;
            if (g.visible) {
                if (g.panelMode == PanelMode::Settings) ShowPanelInternal(PanelMode::Devices);
                else HidePanelInternal();
            } else {
                ShowPanelInternal(PanelMode::Devices);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            g.dragArmed = false;
            g.dragMoved = false;
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                const bool shift = GetKeyState(VK_SHIFT) < 0;
                const bool ctrl = GetKeyState(VK_CONTROL) < 0;
                const bool free = ctrl || (g.settings->pillUnlocked && !shift);
                SetCursor(LoadCursorW(nullptr, free || g.dragFree ? IDC_SIZEALL
                                         : (shift || g.dragArmed) ? IDC_SIZEWE : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSELEAVE:
            g.pillHover = false;
            KillTimer(h, kPillTipTimer);
            HideTip();
            RenderPill();
            // Leaving the pill also counts as "away": the panel closes shortly unless the cursor
            // lands on it (then the panel's own WM_MOUSEMOVE cancels the timer).
            if (g.visible && g.panel) SetTimer(g.panel, kPanelTimerHide, 200, nullptr);
            return 0;
        case WM_RBUTTONUP:
            HideTip();
            KillTimer(h, kPillTipTimer);
            ShowPanelInternal(PanelMode::Settings);
            return 0;
        case WM_APP_REFRESH_UI:
        case WM_DISPLAYCHANGE:
            Tick();
            return 0;
        default:
            if (g.taskbarCreatedMessage && msg == g.taskbarCreatedMessage) {
                Tick();
                return 0;
            }
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ToggleDeviceHidden(int cardIndex) {
    if (cardIndex < 0 || (size_t)cardIndex >= g.cards.size()) return;
    const std::wstring id = g.cards[(size_t)cardIndex].reading.id;
    auto& hidden = g.settings->hiddenDevices;
    auto it = std::find(hidden.begin(), hidden.end(), id);
    if (it == hidden.end()) hidden.push_back(id);
    else hidden.erase(it);
    if (g.hooks.settingsApplied) g.hooks.settingsApplied();
    RenderPanel();
}

LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            KillTimer(h, kPanelTimerHide);
            TrackLeave(h);
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (g.panelMode == PanelMode::Settings) {
                int hit = -1;
                for (size_t i = 0; i < g.settingHits.size(); i++) {
                    if (g.settingHits[i].rect.Contains((float)p.x, (float)p.y)) {
                        hit = (int)i;
                        break;
                    }
                }
                if (hit != g.hoveredSetting) {
                    g.hoveredSetting = hit;
                    RenderPanel();
                }
                return 0;
            }
            int cardIndex = -1;
            for (size_t i = 0; i < g.cardRects.size(); i++) {
                if (g.cardRects[i].Contains((float)p.x, (float)p.y)) {
                    cardIndex = (int)i;
                    break;
                }
            }
            int eyeIndex = -1;
            for (size_t i = 0; i < g.eyeRects.size(); i++) {
                if (g.eyeRects[i].Contains((float)p.x, (float)p.y)) {
                    eyeIndex = (int)i;
                    break;
                }
            }
            if (cardIndex != g.hoveredCard || eyeIndex != g.hoveredEye) {
                g.hoveredCard = cardIndex;
                g.hoveredEye = eyeIndex;
                RenderPanel();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g.hoveredCard = -1;
            g.hoveredEye = -1;
            g.hoveredSetting = -1;
            SetTimer(h, kPanelTimerHide, 200, nullptr);
            RenderPanel();
            return 0;
        case WM_TIMER:
            if (wp == kPanelTimerAnim) {
                Animate();
                return 0;
            }
            if (wp == kPanelTimerHide) {
                KillTimer(h, kPanelTimerHide);
                RECT panelRect{}, pillRect = g.pillRect;
                GetWindowRect(h, &panelRect);
                if (!CursorInside(h, panelRect) && !CursorInside(g.pill, pillRect))
                    HidePanelInternal();
            }
            return 0;
        case WM_LBUTTONUP:
            if (g.panelMode == PanelMode::Devices) {
                const POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                for (size_t i = 0; i < g.eyeRects.size(); i++) {
                    if (g.eyeRects[i].Contains((float)p.x, (float)p.y)) {
                        ToggleDeviceHidden((int)i);
                        break;
                    }
                }
            } else if (g.panelMode == PanelMode::Settings) {
                const POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                for (const auto& hit : g.settingHits) {
                    if (hit.rect.Contains((float)p.x, (float)p.y)) {
                        ApplySettingHit(hit);
                        break;
                    }
                }
            }
            return 0;
        case WM_APP_REFRESH_UI:
        case WM_DISPLAYCHANGE:
            Tick();
            return 0;
        default:
            if (g.taskbarCreatedMessage && msg == g.taskbarCreatedMessage) {
                Tick();
                return 0;
            }
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK TipProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, msg, wp, lp);
}

HWND CreateLayeredWindow(const wchar_t* cls, WNDPROC proc, const wchar_t* title, DWORD extraStyle = 0) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = g.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    HWND wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | extraStyle,
                               cls, title, WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g.inst, nullptr);
    return wnd;
}

} // namespace

std::wstring FullscreenReport() { return FullscreenReportImpl(); }

// ------------------------------------------------------------------ public

bool Init(HINSTANCE inst, Settings* settings, Hooks hooks) {
    g.inst = inst;
    g.settings = settings;
    g.hooks = hooks;


    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &input, nullptr);
    dwtext::Init();
    // Three static Inter faces are embedded as resources and registered privately, so the panel
    // looks the same on Windows 10 and 11 without installing anything.
    if (g.fontData[0].Load(inst, 201) && g.fontData[1].Load(inst, 202) && g.fontData[2].Load(inst, 203)) {
        g.fontCollection = std::make_unique<PrivateFontCollection>();
        for (auto& font : g.fontData) g.fontCollection->AddMemoryFont(font.bytes.data(), (INT)font.bytes.size());
        const INT familyCount = g.fontCollection->GetFamilyCount();
        if (familyCount > 0) {
            std::vector<FontFamily> families((size_t)familyCount);
            INT found = 0;
            if (g.fontCollection->GetFamilies(familyCount, families.data(), &found) == Ok) {
                for (INT i = 0; i < found; i++) {
                    wchar_t name[LF_FACESIZE] = {};
                    families[(size_t)i].GetFamilyName(name);
                    FontFamily* clone = families[(size_t)i].Clone();
                    if (!clone) continue;
                    if (!wcscmp(name, L"ChargeHover Text")) g.familyRegular = clone;
                    else if (!wcscmp(name, L"ChargeHover Medium")) g.familyMedium = clone;
                    else if (!wcscmp(name, L"ChargeHover Semibold")) g.familySemibold = clone;
                    else delete clone;
                }
            }
        }
    }

    g.uiFontVariable = true;
    {
        FontFamily probe(L"Segoe UI Variable Text");
        g.uiFontVariable = probe.GetLastStatus() == Ok;
    }

    BuildTheme();

    for (int id : { icoBatteryCharge,
                    icoMouseFill, icoKeyboardFill, icoHeadphonesFill, icoGamepadFill, icoSpeakerFill,
                    icoPhoneFill })
        g.icons.Load(inst, id);
    for (int i = 0; i <= 10; i++) g.icons.Load(inst, icoBattery0 + i);

    g.panel = CreateLayeredWindow(L"WattPillPanel", PanelProc, L"WattPill panel");
    g.pill = CreateLayeredWindow(L"WattPillPill", PillProc, L"WattPill");
    g.tip = CreateLayeredWindow(L"WattPillTip", TipProc, L"WattPill hint", WS_EX_TRANSPARENT);
    if (!g.pill || !g.panel || !g.tip) return false;

    g.taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    g.winEventHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                     OnForegroundEvent, 0, 0,
                                     WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    RebuildFonts();

    if (g.settings->showPill) {
        ShowWindow(g.pill, SW_SHOWNOACTIVATE);
        RenderPill();
    }
    return true;
}

// A layered window's last surface can stay in the DWM composition until something repaints the
// area (very visible on a translucent taskbar). Wipe the surface to fully transparent first, then
// hide, flush, destroy and force the taskbar to repaint.
void ClearLayered(HWND wnd) {
    if (!wnd) return;
    RECT r{};
    if (!GetWindowRect(wnd, &r)) return;
    const int w = std::max(1, (int)(r.right - r.left));
    const int h = std::max(1, (int)(r.bottom - r.top));

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        ZeroMemory(bits, (size_t)w * h * 4);
        HGDIOBJ old = SelectObject(mem, dib);
        POINT dst{ r.left, r.top }, src{ 0, 0 };
        SIZE size{ w, h };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 0, AC_SRC_ALPHA };
        UpdateLayeredWindow(wnd, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void Shutdown() {
    if (g.winEventHook) {
        UnhookWinEvent(g.winEventHook);
        g.winEventHook = nullptr;
    }
    if (g.pill) {
        ClearLayered(g.pill);
        ShowWindow(g.pill, SW_HIDE);
        DestroyWindow(g.pill);
        g.pill = nullptr;
    }
    if (g.panel) {
        ClearLayered(g.panel);
        ShowWindow(g.panel, SW_HIDE);
        DestroyWindow(g.panel);
        g.panel = nullptr;
    }
    if (g.tip) {
        ClearLayered(g.tip);
        ShowWindow(g.tip, SW_HIDE);
        DestroyWindow(g.tip);
        g.tip = nullptr;
    }
    DwmFlush();
    if (HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr))
        RedrawWindow(bar, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
    DwmFlush();
    dwtext::Shutdown();
}

void SetCards(std::vector<Card> cards) {
    g.cards = std::move(cards);
    for (const auto& card : g.cards) {
        if (!g.ringValues.count(card.reading.id)) g.ringValues[card.reading.id] = TargetPercent(card);
    }

    // Keep the pill's height stable even when there is nothing to show.
    if (g.settings->showPill && g.pill) {
        ShowWindow(g.pill, SW_SHOWNOACTIVATE);
        RenderPill();
    } else if (g.pill) {
        ShowWindow(g.pill, SW_HIDE);
    }
    if (g.visible) RenderPanel();
}

void Tick() {
    if (!g.pill || !g.panel) return;

    const bool fs = g.settings->hideFullscreen && FullscreenApp();
    if (fs != g.fullscreen) {
        g.fullscreen = fs;
        if (fs) {
            ShowWindow(g.pill, SW_HIDE);
            HideTip();
            HidePanelInternal();
        } else if (g.settings->showPill && !g.taskbarHidden) {
            ShowWindow(g.pill, SW_SHOWNOACTIVATE);
            RenderPill();
        }
    }
    if (g.fullscreen) return;

    // Layered popups cannot be pinned above the Windows 11 taskbar: there is no supported API for
    // that, and Windows raises its own topmost taskbar on screenshots and focus changes. Walk the
    // z-order (read-only) and only re-assert when the taskbar really got above us.
    const BarLayout layout = BarForMonitor(MonitorAt(g.settings->monitorIndex));
    if (!IsWindow(g.pill)) {
        g.pill = CreateLayeredWindow(L"WattPillPill", PillProc, L"WattPill");
        if (!g.pill) return;
        ShowWindow(g.pill, SW_SHOWNOACTIVATE);
        RenderPill();
    }
    RECT bar = layout.bar;
    const RECT monRect = layout.monitor;
    const bool barHidden = !g.settings->pillFree && layout.found &&
                           (!IsWindowVisible(layout.taskbar) || bar.right <= bar.left || bar.bottom <= bar.top ||
                            bar.top >= monRect.bottom - 2 || bar.bottom <= monRect.top + 2);
    if (barHidden != g.taskbarHidden) {
        g.taskbarHidden = barHidden;
        if (barHidden) {
            ShowWindow(g.pill, SW_HIDE);
            HideTip();
            HidePanelInternal();
        } else if (g.settings->showPill && !g.fullscreen) {
            ShowWindow(g.pill, SW_SHOWNOACTIVATE);
            RenderPill();
        }
    }
    if (g.taskbarHidden) return;
    static RECT lastBar{};
    if (layout.found && memcmp(&bar, &lastBar, sizeof(RECT)) != 0) {
        lastBar = bar;
        RenderPill();
        if (g.visible) RenderPanel();
    }
        // Ask Windows which window would receive a click on the pill. If none of several points
        // along the strip belongs to us, something (the taskbar after Start or a screenshot, a
        // shell flyout) is above the pill, so put it back.
        if (g.pillRect.right > g.pillRect.left && g.pillRect.bottom > g.pillRect.top) {
            const int cy = (g.pillRect.top + g.pillRect.bottom) / 2;
            const int inset = std::min(12, (int)(g.pillRect.right - g.pillRect.left) / 2);
            const int xs[] = { g.pillRect.left + inset, (g.pillRect.left + g.pillRect.right) / 2,
                               g.pillRect.right - inset };
            bool onTop = false;
            for (int x : xs) {
                POINT probe{ x, cy };
                if (WindowFromPoint(probe) == g.pill) { onTop = true; break; }
            }
            if (!onTop) {
                SetWindowPos(g.pill, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                if (g.visible)
                    SetWindowPos(g.panel, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }

    RECT panelRect{};
    (void)panelRect;
}

void SetSettings(const Settings& settings) {
    if (!g.settings) return;
    *g.settings = settings;
    BuildTheme();
    RebuildFonts();
    if (g.settings->showPill && !g.taskbarHidden && !g.fullscreen) {
        ShowWindow(g.pill, SW_SHOWNOACTIVATE);
        RenderPill();
    } else {
        ShowWindow(g.pill, SW_HIDE);
    }
    if (g.visible) RenderPanel();
}

void OnSystemSettingsChanged() {
    BuildTheme();
    if (g.pill) RenderPill();
    if (g.panel && g.visible) RenderPanel();
}

void ShowPanelNow() { ShowPanelInternal(PanelMode::Devices); }
void ShowSettingsNow() { ShowPanelInternal(PanelMode::Settings); }
void TogglePanelNow() { if (g.visible) HidePanelInternal(); else ShowPanelInternal(PanelMode::Devices); }

std::wstring FontReport() {
    using namespace Gdiplus;
    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &input, nullptr);

    std::wstring out;
    {
        BundledFont data[3];
        const int ids[3] = { 201, 202, 203 };
        bool ok = true;
        for (int i = 0; i < 3; i++) ok = data[i].Load(GetModuleHandleW(nullptr), ids[i]) && ok;
        out += Fmt(L"resources: %s\n", ok ? L"loaded" : L"missing");
        if (ok) {
            PrivateFontCollection collection;
            for (auto& f : data) collection.AddMemoryFont(f.bytes.data(), (INT)f.bytes.size());
            const INT count = collection.GetFamilyCount();
            out += Fmt(L"private families: %d\n", (int)count);
            if (count > 0) {
                std::vector<FontFamily> families((size_t)count);
                INT found = 0;
                if (collection.GetFamilies(count, families.data(), &found) == Ok) {
                    for (INT i = 0; i < found; i++) {
                        wchar_t name[LF_FACESIZE] = {};
                        families[(size_t)i].GetFamilyName(name);
                        out += std::wstring(L"  ") + name + L"\n";
                    }
                }
            }
            FontFamily segoe(L"Segoe UI");
            out += Fmt(L"system Segoe UI: %s\n", segoe.GetLastStatus() == Ok ? L"ok" : L"missing");
        }
    }
    GdiplusShutdown(token);
    return out;
}
} // namespace ui
} // namespace ch

