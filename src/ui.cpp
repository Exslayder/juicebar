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
    std::unique_ptr<Font> name, small, status, percent;
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

Color PercentColor(const Card& card) {
    if (IsLow(card)) return g.theme.red;
    if (card.reading.charging) return g.theme.charging;
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

    if (g.taskbarHidden && !g.settings->pillFree) {
        ShowWindow(g.pill, SW_HIDE);
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
        if (card.reading.connected) shown.push_back(&card);

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
            addText(KindGlyph(card.reading.kind), g.fonts.pillGlyph.get(), dwtext::Face::Glyph,
                    13.5f * g.scale, 400,
                    RectF(item.x, cy - 9 * g.scale, item.glyphW + 2 * g.scale, 18 * g.scale),
                    WithAlpha(off ? g.theme.pillSub : g.theme.pillInk, alpha), StringAlignmentNear);
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

void DrawBatteryLevel(Graphics& g2, RectF box, const Card& card, float shownPercent) {
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
    const bool hovered = g.hoveredCard >= 0 && (size_t)g.hoveredCard < g.cards.size() &&
                         &card == &g.cards[(size_t)g.hoveredCard];
    FillRound(g2, box, 16 * g.scale, hovered ? g.theme.cardHover : g.theme.card);

    const float glyphX = box.X + 16 * g.scale;
    const float glyphSize = 22 * g.scale;
    Color glyphColor = card.reading.charging ? g.theme.green : IsLow(card) ? g.theme.red : g.theme.text;
    if (card.reading.asleep || !card.reading.connected) glyphColor = g.theme.faint;
    g.icons.Draw(g2, IconFor(card.reading.kind), RectF(glyphX, box.Y + 16 * g.scale, glyphSize, glyphSize),
                 glyphColor, card.reading.asleep ? 150 : 255);

    const float right = box.GetRight() - 16 * g.scale;
    const float battW = 24 * g.scale;
    const float pctW = 72 * g.scale;
    const float textX = glyphX + glyphSize + 12 * g.scale;
    const float textW = right - battW - 8 * g.scale - pctW - 10 * g.scale - textX;
    const BYTE textAlpha = card.reading.asleep || !card.reading.connected ? 165 : 255;

    Text(g2, card.reading.name, g.fonts.name.get(), RectF(textX, box.Y + 10 * g.scale, textW, 22 * g.scale),
         WithAlpha(g.theme.text, textAlpha), StringAlignmentNear, true);

    Color statusColor = g.theme.sub;
    if (card.reading.charging) statusColor = g.theme.green;
    else if (IsLow(card)) statusColor = g.theme.red;
    else if (card.forecast.state == ForecastState::Ready || card.forecast.state == ForecastState::LowConfidence)
        statusColor = g.theme.accent;
    Text(g2, StatusLine(card), g.fonts.status.get(), RectF(textX, box.Y + 33 * g.scale, textW, 20 * g.scale),
         WithAlpha(statusColor, textAlpha), StringAlignmentNear, true);

    // Ready-made battery icons (Fluent): the level morphs between two neighbouring icons, and the
    // number counts up, so a level change reads as motion instead of a jump.
    const float shown = g.ringValues.count(card.reading.id) ? g.ringValues[card.reading.id]
                                                           : TargetPercent(card);
    const RectF batt(right - battW, box.Y + 11 * g.scale, battW, battW);
    DrawBatteryLevel(g2, batt, card, shown);
    Text(g2, Fmt(L"%d%%", (int)std::lround(shown)), g.fonts.percent.get(),
         RectF(right - battW - 8 * g.scale - pctW, box.Y + 8 * g.scale, pctW, 28 * g.scale),
         WithAlpha(PercentColor(card), textAlpha), StringAlignmentFar);
}

void RenderPanel() {
    if (!g.panel) return;
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

void ShowPanelInternal() {
    g.visible = true;
    if (!g.panel) return;
    HideTip();
    if (!g.reveal) g.reveal = 0.0f;
    g.animLast = 0;
    RenderPanel();
    ShowWindow(g.panel, SW_SHOWNOACTIVATE);
    SetWindowPos(g.panel, HWND_TOPMOST, g.panelPos.x, g.panelPos.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(g.panel, kPanelTimerAnim, 33, nullptr);
}

void HidePanelInternal() {

    g.visible = false;
    g.hoveredCard = -1;
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
            if (g.visible) HidePanelInternal();
            else ShowPanelInternal();
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
            if (g.hooks.menu) g.hooks.menu();
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

LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            KillTimer(h, kPanelTimerHide);
            TrackLeave(h);
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int cardIndex = -1;
            for (size_t i = 0; i < g.cardRects.size(); i++) {
                if (g.cardRects[i].Contains((float)p.x, (float)p.y)) {
                    cardIndex = (int)i;
                    break;
                }
            }
            if (cardIndex != g.hoveredCard) {
                g.hoveredCard = cardIndex;
                RenderPanel();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g.hoveredCard = -1;
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

void ShowPanelNow() { ShowPanelInternal(); }
void TogglePanelNow() { if (g.visible) HidePanelInternal(); else ShowPanelInternal(); }

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

