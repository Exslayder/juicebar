// JuiceBar UI: a compact taskbar pill plus a hover panel with per-device battery
// forecast. Everything is drawn by hand on a premultiplied 32-bit surface and pushed with
// UpdateLayeredWindow, so the pill and the panel get real rounded corners and soft alpha.
#include "ui.h"

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

constexpr int kPillTimerHover = 1;
constexpr int kPanelTimerHide = 2;
constexpr int kPanelTimerAnim = 3;

struct Theme {
    Color bg{ 250, 23, 24, 29 };
    Color bgSolid{ 255, 23, 24, 29 };
    Color card{ 12, 255, 255, 255 };
    Color cardHover{ 24, 255, 255, 255 };
    Color border{ 26, 255, 255, 255 };
    Color text{ 255, 242, 243, 247 };
    Color sub{ 255, 150, 156, 170 };
    Color faint{ 255, 104, 110, 124 };
    Color track{ 34, 255, 255, 255 };
    Color accent{ 255, 124, 92, 255 };
    Color red{ 255, 248, 113, 113 };
    Color green{ 255, 74, 222, 128 };
    Color blue{ 255, 96, 165, 250 };
    Color amber{ 255, 251, 191, 36 };
    bool light = false;
};

struct Fonts {
    // Bundled faces (Inter, OFL) are used when available; Segoe UI is the system fallback.
    std::unique_ptr<FontFamily> fallbackUi;
    std::unique_ptr<FontFamily> fallbackSemi;
    std::unique_ptr<FontFamily> iconFamily;
    std::unique_ptr<FontFamily> pillFamily;
    std::unique_ptr<Font> title, name, body, small, tiny, status, percent, glyph, iconSmall, pillGlyph, pillPct, ringPct;

    // Inter runs a touch wider than Segoe UI at the same size, so the scale is slightly smaller.
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

        // MDL2 codepoints are stable on Windows 10 and 11; Fluent Icons remaps several of them.
        iconFamily = std::make_unique<FontFamily>(L"Segoe MDL2 Assets");
        if (iconFamily->GetLastStatus() != Ok) iconFamily = std::make_unique<FontFamily>(L"Segoe UI Symbol");

        auto make = [&](std::unique_ptr<Font>& f, int weight, float px, bool icon) {
            FontFamily* family = icon ? iconFamily.get() : weight == 2 ? semibold : weight == 1 ? medium : regular;
            f = std::make_unique<Font>(family, px * scale, FontStyleRegular, UnitPixel);
        };
        make(title, 2, 15.5f, false);
        make(name, 2, 14.5f, false);
        make(body, 1, 13.5f, false);
        make(status, 2, 13.5f, false);
        make(small, 0, 12.0f, false);
        make(tiny, 0, 11.0f, false);
        make(percent, 2, 19.0f, false);
        make(glyph, 0, 17.0f, true);
        make(iconSmall, 0, 12.0f, true);
        make(pillGlyph, 0, 15.0f, true);
        // The taskbar text is small: Segoe UI hints better at 13 px than Inter, and an integer
        // size keeps GDI+ from blurring the strokes.
        pillFamily = std::make_unique<FontFamily>(L"Segoe UI Semibold");
        if (pillFamily->GetLastStatus() != Ok) pillFamily = std::make_unique<FontFamily>(L"Segoe UI");
        pillPct = std::make_unique<Font>(pillFamily.get(), 13.0f * scale, FontStyleRegular, UnitPixel);
        make(ringPct, 2, 16.0f, false);
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
    int64_t polledAt = 0;

    bool pillHover = false;
    bool panelHover = false;
    bool visible = false;
    bool hideWhenCursorLeaves = true;

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
    bool fontsForLight = false;

    int pillW = 0, pillH = 0;
    int panelW = 0, panelH = 0;
    std::map<std::wstring, float> ringValues;
    float spin = 0;
    float hoverGlow = 0;
    float reveal = 0;
    ULONGLONG animLast = 0;
    POINT panelPos{};
    RECT pillRect{};
    std::vector<RectF> cardRects;
    bool fullscreen = false;
    ULONGLONG panelShownTick = 0;

    bool iconFontFluent = true;
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

Color AccentColor(int index) {
    static const Color accents[] = {
        Color(255, 124, 92, 255),   // violet
        Color(255, 29, 185, 84),    // Spotify green
        Color(255, 229, 9, 20),     // Netflix red
        Color(255, 10, 132, 255),   // Apple blue
        Color(255, 244, 114, 182),  // pink
    };
    return accents[std::clamp(index, 0, 4)];
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
    return card.reading.connected && card.reading.percent >= 0 && card.reading.percent <= 15 && !card.reading.charging;
}

Color PercentColor(const Card& card) {
    if (IsLow(card)) return g.theme.red;
    if (card.reading.charging) return g.theme.green;
    if (card.reading.asleep || !card.reading.connected) return g.theme.faint;
    return g.theme.text;
}

int NominalHours(Kind kind) {
    switch (kind) {
        case Kind::Headset: return 40;
        case Kind::Earbuds: return 8;
        case Kind::Speaker: return 12;
        case Kind::Mouse: return 70;
        case Kind::Keyboard: return 200;
        case Kind::Gamepad: return 30;
        default: return 24;
    }
}

std::wstring StatusLine(const Card& card) {
    const Reading& r = card.reading;
    const Forecast& f = card.forecast;
    if (!r.connected) return Tr(L"Не подключено", L"Not connected");
    if (r.asleep) return Tr(L"Нет ответа · последний замер", L"No answer · last reading");

    if (r.charging) {
        if (f.state == ForecastState::Charging && f.minutes >= 0)
            return std::wstring(Tr(L"Заряжается · до полного ", L"Charging · ")) + FormatSpan(f.minutes);
        if (r.full || r.percent >= 100) return Tr(L"Полностью заряжено", L"Fully charged");
        return Tr(L"Заряжается", L"Charging");
    }

    if (r.percent > 0 && (f.state == ForecastState::Learning || f.state == ForecastState::Flat ||
                          f.state == ForecastState::NoData)) {
        const int minutes = (int)(r.percent * (NominalHours(r.kind) * 60) / 100);
        return std::wstring(L"≈ ") + FormatSpan(minutes);
    }

    switch (f.state) {
        case ForecastState::Ready:
        case ForecastState::LowConfidence: {
            return std::wstring(L"≈ ") + FormatSpan(f.minutes);
        }
        case ForecastState::Flat:
            return Tr(L"Расход стабилен", L"Drain is steady");
        case ForecastState::Learning:
            return Tr(L"Калибровка", L"Calibrating");
        default:
            return Tr(L"Ожидание данных", L"Waiting for data");
    }
}

// ------------------------------------------------------------------ drawing: pill

struct PillItem {
    const Card* card = nullptr;
    float x = 0;
    float glyphW = 0;
    float textW = 0;
};

void RenderPill() {
    if (!g.pill) return;

    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND notify = taskbar ? FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    if (!taskbar || !notify) return;
    RECT barRect{}, notifyRect{};
    GetWindowRect(taskbar, &barRect);
    GetWindowRect(notify, &notifyRect);
    const int barW = barRect.right - barRect.left, barH = barRect.bottom - barRect.top;
    if (barW <= barH) return;   // vertical taskbar
    const int height = barH;    // the pill and its hover fill the taskbar height
    const float pad = 10 * g.scale;
    const float gap = 9 * g.scale;
    const float iconGap = 4 * g.scale;

    // Only devices that have a level or are disconnected show up on the taskbar.
    std::vector<const Card*> shown;
    for (const auto& card : g.cards)
        if (card.reading.connected || card.reading.percent >= 0) shown.push_back(&card);

    Canvas probe(4, 4);
    Graphics& pg = *probe.g;

    std::vector<PillItem> items;
    float x = pad;
    for (const Card* card : shown) {
        PillItem item;
        item.card = card;
        item.x = x;
        item.glyphW = 16 * g.scale;
        item.textW = TextWidth(pg, PercentText(*card), g.fonts.pillPct.get());
        items.push_back(item);
        x += item.glyphW + iconGap + item.textW;
        x += gap;
    }
    const bool empty = items.empty();
    float emptyGlyphW = 16 * g.scale;
    if (empty) x = pad + emptyGlyphW + iconGap + g.fonts.pillPct->GetSize() * 1.2f + gap;

    const int width = std::max((int)std::lround(x - gap + pad), (int)std::lround(46 * g.scale));
    Canvas canvas(width, height);
    Graphics& g2 = *canvas.g;
    g2.SetPixelOffsetMode(PixelOffsetModeNone);

    // The pill lives on the taskbar, so its contrast follows the taskbar, not the panel theme.
    const bool barLight = TaskbarIsLight();
    const Color ink = barLight ? Color(255, 22, 22, 26) : Color(255, 246, 247, 250);
    const Color dimInk = barLight ? Color(255, 70, 70, 76) : Color(255, 168, 170, 178);
    const Color pillGreen = barLight ? Color(255, 30, 150, 70) : g.theme.green;
    const Color pillRed = barLight ? Color(255, 200, 30, 30) : g.theme.red;

    // Alpha 1 over the whole strip: invisible on screen, but it keeps the window clickable.
    FillRound(g2, RectF(0, 0, (float)width, (float)height), height / 2.0f, Color(8, 0, 0, 0));

    // Light hover wash, inset from the strip edges so it reads as a small rounded button instead
    // of a full-height block (the tray hover is ~32 px on a 48 px taskbar).
    if (g.pillHover) {
        const float insetX = 3 * g.scale, insetY = 8 * g.scale;
        FillRound(g2, RectF(insetX, insetY, width - 2 * insetX, height - 2 * insetY), 4 * g.scale,
                  WithAlpha(barLight ? Color(255, 0, 0, 0) : Color(255, 255, 255, 255),
                            barLight ? 10 : 12));
    }

    const float cy = height / 2.0f;
    if (empty) {
        SolidBrush dot(dimInk);
        for (int i = 0; i < 3; i++)
            g2.FillEllipse(&dot, RectF(pad + i * 10 * g.scale, cy - 2 * g.scale, 4 * g.scale, 4 * g.scale));
    }

    for (const auto& item : items) {
        const Card& card = *item.card;
        const BYTE alpha = card.reading.asleep ? 110 : 255;
        Color iconColor = ink;
        if (IsLow(card)) iconColor = pillRed;
        else if (card.reading.charging || card.reading.full) iconColor = pillGreen;
        if (card.reading.asleep) iconColor = dimInk;
        g.icons.Draw(g2, IconFor(card.reading.kind),
                     RectF(item.x, (height - item.glyphW) / 2, item.glyphW, item.glyphW),
                     iconColor, alpha);

        const float textX = item.x + item.glyphW + iconGap;
        Text(g2, PercentText(card), g.fonts.pillPct.get(),
             RectF(textX, cy - 10 * g.scale, item.textW + 2, 17 * g.scale),
             WithAlpha(IsLow(card) ? pillRed : card.reading.charging ? pillGreen
                       : card.reading.asleep ? dimInk : ink, alpha),
             StringAlignmentNear);

    }

    // Float just above the clock; the right edge stays put and the pill grows to the left as
    // devices connect, so it never covers taskbar buttons, toolbars or widgets.
    const int pillRight = notifyRect.right - (int)std::lround(40 * g.scale);
    const int px = pillRight - width;
    const int py = barRect.top - height - (int)std::lround(4 * g.scale);
    if (px < barRect.left + 8) return;
    g.pillRect = { px, py, px + width, py + height };
    g.pillW = width;
    g.pillH = height;
    canvas.Present(g.pill, px, py);
}

// ------------------------------------------------------------------ drawing: panel

int MeasurePanelHeight(int count) {
    const int pad = (int)std::lround(16 * g.scale);
    const int header = 0;
    const int cardH = (int)std::lround(80 * g.scale);
    const int gap = (int)std::lround(10 * g.scale);
    const int footer = 0;
    const int body = count > 0 ? count * cardH + (count - 1) * gap : (int)std::lround(84 * g.scale);
    return pad + header + body + footer + pad;
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

    // ---- footer


    // ---- place above the notification area
    RECT bar{}, notify{};
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND tray = taskbar ? FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    int panelX = 0, panelY = 0;
    if (taskbar && tray) {
        GetWindowRect(taskbar, &bar);
        GetWindowRect(tray, &notify);
        const int anchorRight = g.pillRect.right > 0 ? g.pillRect.right : notify.right;
        panelX = anchorRight - width - (int)std::lround(6 * g.scale);
        if (bar.top < 40) panelY = bar.bottom + (int)std::lround(8 * g.scale);
        else if (g.pillRect.top > 0 && g.pillRect.top < bar.top)
            panelY = g.pillRect.top - height - (int)std::lround(8 * g.scale);
        else panelY = bar.top - height - (int)std::lround(8 * g.scale);
    } else {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        panelX = work.right - width - 12;
        panelY = work.bottom - height - 12;
    }
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    panelX = std::clamp(panelX, (int)work.left + 8, (int)work.right - width - 8);
    panelY = std::clamp(panelY, (int)work.top + 8, (int)work.bottom - height - 8);

    g.panelW = width;
    g.panelH = height;
    g.panelPos = { panelX, panelY };
    canvas.Present(g.panel, panelX, panelY, (BYTE)std::lround(255 * std::clamp(g.reveal, 0.0f, 1.0f)));
}

// ------------------------------------------------------------------ theme & windows

void BuildTheme() {
    const bool light = g.settings->theme == 2 || (g.settings->theme == 0 && TaskbarIsLight());
    g.theme.light = light;
    g.theme.accent = AccentColor(g.settings->accent);
    if (light) {
        g.theme.bg = Color(242, 249, 249, 249);
        g.theme.bgSolid = Color(255, 249, 249, 249);
        g.theme.card = Color(255, 255, 255, 255);
        g.theme.cardHover = Color(255, 239, 239, 239);
        g.theme.border = Color(30, 60, 60, 67);
        g.theme.text = Color(255, 17, 17, 19);
        g.theme.sub = Color(255, 108, 108, 116);
        g.theme.faint = Color(255, 142, 142, 150);
        g.theme.track = Color(30, 60, 60, 67);
        g.theme.red = Color(255, 215, 0, 21);
        g.theme.green = Color(255, 36, 138, 61);
        g.theme.blue = Color(255, 0, 122, 255);
    } else {
        g.theme.bg = Color(236, 32, 32, 32);
        g.theme.bgSolid = Color(255, 32, 32, 32);
        g.theme.card = Color(255, 43, 43, 43);
        g.theme.cardHover = Color(255, 51, 51, 51);
        g.theme.border = Color(26, 255, 255, 255);
        g.theme.text = Color(255, 255, 255, 255);
        g.theme.sub = Color(255, 161, 161, 166);
        g.theme.faint = Color(255, 124, 124, 130);
        g.theme.track = Color(31, 255, 255, 255);
        g.theme.red = Color(255, 255, 69, 58);
        g.theme.green = Color(255, 48, 209, 88);
        g.theme.blue = Color(255, 10, 132, 255);
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
    if (!g.reveal) g.reveal = 0.0f;
    g.animLast = 0;
    ShowWindow(g.panel, SW_SHOWNOACTIVATE);
    g.panelShownTick = GetTickCount64();
    ShowWindow(g.panel, SW_SHOWNOACTIVATE);
    SetWindowPos(g.panel, HWND_TOPMOST, g.panelPos.x, g.panelPos.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(g.panel, kPanelTimerAnim, 33, nullptr);
    RenderPanel();
}

void HidePanelInternal() {

    g.visible = false;
    g.panelHover = false;
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

// Borderless ("fullscreen windowed") games cover the whole monitor, including the strip the
// taskbar lives on, but Windows still reports a normal notification state. Detect the window
// itself: if the foreground window covers the monitor of our taskbar, treat it as fullscreen.
bool ForegroundCoversTaskbar() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    if (pid == GetCurrentProcessId()) return false;

    wchar_t cls[64] = {};
    GetClassNameW(foreground, cls, 64);
    if (!_wcsicmp(cls, L"Progman") || !_wcsicmp(cls, L"WorkerW") ||
        !_wcsicmp(cls, L"Shell_TrayWnd") || !_wcsicmp(cls, L"Shell_SecondaryTrayWnd"))
        return false;   // the desktop or the taskbar itself are never a game

    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) return false;
    HMONITOR monitor = MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{ sizeof(info) };
    if (!GetMonitorInfoW(monitor, &info)) return false;

    RECT window{};
    if (!GetWindowRect(foreground, &window)) return false;
    const RECT& screen = info.rcMonitor;
    const LONG tolerance = 2;
    return window.left <= screen.left + tolerance && window.top <= screen.top + tolerance &&
           window.right >= screen.right - tolerance && window.bottom >= screen.bottom - tolerance;
}

bool FullscreenApp() {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
        (state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE))
        return true;
    return ForegroundCoversTaskbar();
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
    const float hoverTarget = g.hoveredCard >= 0 ? 1.0f : 0.0f;
    if (std::fabs(hoverTarget - g.hoverGlow) > 0.01f) {
        g.hoverGlow += (hoverTarget - g.hoverGlow) * std::min(1.0f, dt * 12.0f);
        active = true;
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
        case WM_MOUSEMOVE:
            if (!g.pillHover) {
                g.pillHover = true;
                TrackLeave(h);
                RenderPill();
                        }
            return 0;
        case WM_MOUSELEAVE:
            g.pillHover = false;
            KillTimer(h, kPillTimerHover);
            RenderPill();
            // Leaving the pill also counts as "away": the panel closes shortly unless the cursor
            // lands on it (then the panel's own WM_MOUSEMOVE cancels the timer).
            if (g.visible && g.panel) SetTimer(g.panel, kPanelTimerHide, 200, nullptr);
            return 0;
        case WM_TIMER:
            if (wp == kPillTimerHover) {
                KillTimer(h, kPillTimerHover);
                if (g.pillHover && !g.visible) ShowPanelInternal();
            }
            return 0;
        case WM_LBUTTONUP: {
            // Debounce: a burst of clicks is one gesture, not a flicker of open/close animations.
            static ULONGLONG lastToggle = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - lastToggle < 300) return 0;
            lastToggle = now;
            if (g.visible) HidePanelInternal();
            else ShowPanelInternal();
            return 0;
        }
        case WM_RBUTTONUP:
            if (g.hooks.menu) g.hooks.menu();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
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
            g.panelHover = false;
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
        case WM_RBUTTONUP:
            if (g.hooks.menu) g.hooks.menu();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND CreateLayeredWindow(const wchar_t* cls, WNDPROC proc, const wchar_t* title) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = g.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    HWND wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls, title,
                               WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g.inst, nullptr);
    return wnd;
}

} // namespace

// ------------------------------------------------------------------ public

bool Init(HINSTANCE inst, Settings* settings, Hooks hooks, const std::wstring& dataDir) {
    g.inst = inst;
    g.settings = settings;
    g.hooks = hooks;


    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &input, nullptr);

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
    {
        FontFamily probe(L"Segoe Fluent Icons");
        g.iconFontFluent = probe.GetLastStatus() == Ok;
    }

    BuildTheme();

    for (int id : { icoBatteryCharge,
                    icoMouseFill, icoKeyboardFill, icoHeadphonesFill, icoGamepadFill, icoSpeakerFill,
                    icoPhoneFill })
        g.icons.Load(inst, id);
    for (int i = 0; i <= 10; i++) g.icons.Load(inst, icoBattery0 + i);

    g.panel = CreateLayeredWindow(L"JuiceBarPanel", PanelProc, L"JuiceBar panel");
    g.pill = CreateLayeredWindow(L"JuiceBarPill", PillProc, L"JuiceBar");
    if (!g.pill || !g.panel) return false;

    RebuildFonts();

    ShowWindow(g.pill, SW_SHOWNOACTIVATE);
    RenderPill();
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
    DwmFlush();
    if (HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr))
        RedrawWindow(bar, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
    DwmFlush();
}

void SetCards(std::vector<Card> cards, int64_t polledAt) {
    g.cards = std::move(cards);
    g.polledAt = polledAt;
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

void SetSettings(const Settings& settings) {
    if (!g.settings) return;
    *g.settings = settings;
    BuildTheme();
    RebuildFonts();
    if (g.settings->showPill) ShowWindow(g.pill, SW_SHOWNOACTIVATE);
    else ShowWindow(g.pill, SW_HIDE);
    RenderPill();
}

void Tick() {
    if (!g.pill || !g.panel) return;

    const bool fs = FullscreenApp();
    if (fs != g.fullscreen) {
        g.fullscreen = fs;
        ShowWindow(g.pill, fs ? SW_HIDE : SW_SHOWNOACTIVATE);
        if (fs) HidePanelInternal();
    }
    if (g.fullscreen) return;

    // Layered popups cannot be pinned above the Windows 11 taskbar: there is no supported API for
    // that, and Windows raises its own topmost taskbar on screenshots and focus changes. Walk the
    // z-order (read-only) and only re-assert when the taskbar really got above us.
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (taskbar) {
        if (!IsWindow(g.pill)) {
            g.pill = CreateLayeredWindow(L"JuiceBarPill", PillProc, L"JuiceBar");
            if (!g.pill) return;
            ShowWindow(g.pill, SW_SHOWNOACTIVATE);
            RenderPill();
        }
        RECT bar{};
        GetWindowRect(taskbar, &bar);
        static RECT lastBar{};
        if (memcmp(&bar, &lastBar, sizeof(RECT)) != 0) {
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
    }

    RECT panelRect{};
    if (g.visible) {
        GetWindowRect(g.panel, &panelRect);
        if (!CursorInside(g.panel, panelRect) && !CursorInside(g.pill, g.pillRect))
            HidePanelInternal();
    }
}

void OnSystemSettingsChanged() {
    BuildTheme();
    if (g.pill) RenderPill();
    if (g.panel && g.visible) RenderPanel();
}

void ShowPanelNow() { ShowPanelInternal(); }

void HidePanel() { HidePanelInternal(); }

bool PanelVisible() { return g.visible; }

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

