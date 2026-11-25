#include "dwrite_text.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace ch {
namespace dwtext {
namespace {

struct State {
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dwrite = nullptr;
    ID2D1DCRenderTarget* target = nullptr;
    ID2D1SolidColorBrush* white = nullptr;
    std::map<std::tuple<int, int, int>, IDWriteTextFormat*> formats;
    bool initialized = false;
};

State g;

IDWriteTextFormat* FormatFor(Face face, float px, int weight) {
    if (!g.dwrite) return nullptr;
    const int pxKey = (int)std::lround(px * 10);
    const auto key = std::make_tuple((int)face, pxKey, weight);
    auto it = g.formats.find(key);
    if (it != g.formats.end()) return it->second;

    IDWriteTextFormat* format = nullptr;
    const wchar_t* family = face == Face::Glyph ? L"Segoe MDL2 Assets" : L"Segoe UI Variable Text";
    HRESULT hr = g.dwrite->CreateTextFormat(family, nullptr, (DWRITE_FONT_WEIGHT)weight,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           px, L"", &format);
    if (FAILED(hr) && face == Face::Ui)
        hr = g.dwrite->CreateTextFormat(L"Segoe UI", nullptr, (DWRITE_FONT_WEIGHT)weight,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        px, L"", &format);
    if (FAILED(hr) || !format) return nullptr;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g.formats[key] = format;
    return format;
}

void BlendMask(BYTE* dst, int stride, int width, int height, float ox, float oy,
               const BYTE* mask, int mw, int mh, uint32_t argb) {
    const int left = (int)std::lround(ox);
    const int top = (int)std::lround(oy);
    const BYTE sr = (BYTE)((argb >> 16) & 0xFF);
    const BYTE sg = (BYTE)((argb >> 8) & 0xFF);
    const BYTE sb = (BYTE)(argb & 0xFF);
    const BYTE sa = (BYTE)((argb >> 24) & 0xFF);

    for (int y = 0; y < mh; y++) {
        const int dy = top + y;
        if (dy < 0 || dy >= height) continue;
        for (int x = 0; x < mw; x++) {
            const int dx = left + x;
            if (dx < 0 || dx >= width) continue;
            const BYTE* m = mask + ((size_t)y * mw + x) * 4;
            const BYTE coverage = std::max({ m[0], m[1], m[2] });
            if (!coverage) continue;
            const BYTE a = (BYTE)((coverage * sa) / 255);
            if (!a) continue;
            const BYTE inv = (BYTE)(255 - a);
            BYTE* d = dst + (size_t)dy * stride + (size_t)dx * 4;
            d[0] = (BYTE)((sb * a) / 255 + (d[0] * inv) / 255);
            d[1] = (BYTE)((sg * a) / 255 + (d[1] * inv) / 255);
            d[2] = (BYTE)((sr * a) / 255 + (d[2] * inv) / 255);
            d[3] = (BYTE)(a + (d[3] * inv) / 255);
        }
    }
}

} // namespace

bool Init() {
    if (g.initialized) return true;

    // D2D/DWrite are COM-based. The UI thread may already have a different apartment; that is fine
    // because the factories are thread-safe and we only use them from this thread.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                                 nullptr, (void**)&g.d2d)))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)&g.dwrite)))
        return false;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(g.d2d->CreateDCRenderTarget(&props, &g.target))) return false;
    g.target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &g.white);

    g.initialized = g.target != nullptr;
    return g.initialized;
}

void Shutdown() {
    for (auto& entry : g.formats) {
        if (entry.second) entry.second->Release();
    }
    g.formats.clear();
    if (g.white) g.white->Release();
    if (g.target) g.target->Release();
    if (g.dwrite) g.dwrite->Release();
    if (g.d2d) g.d2d->Release();
    g.white = nullptr;
    g.target = nullptr;
    g.dwrite = nullptr;
    g.d2d = nullptr;
    g.initialized = false;
}

bool Available() { return g.initialized; }

void DrawRuns(void* premultipliedBits, int stride, int width, int height,
              const std::vector<Run>& runs) {
    if (!g.initialized || !premultipliedBits || runs.empty()) return;
    auto* dst = static_cast<BYTE*>(premultipliedBits);

    for (const Run& run : runs) {
        if (run.text.empty()) continue;
        const int w = std::max(1, (int)std::ceil(run.w));
        const int h = std::max(1, (int)std::ceil(run.h));

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) {
            if (dib) DeleteObject(dib);
            DeleteDC(mem);
            ReleaseDC(nullptr, screen);
            continue;
        }
        memset(bits, 0, (size_t)w * h * 4);
        HGDIOBJ old = SelectObject(mem, dib);

        RECT rc{ 0, 0, w, h };
        if (SUCCEEDED(g.target->BindDC(mem, &rc))) {
            IDWriteTextFormat* format = FormatFor(run.face, run.px, run.weight);
            if (format) {
                format->SetTextAlignment(run.align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER
                                        : run.align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                         : DWRITE_TEXT_ALIGNMENT_LEADING);
                g.target->BeginDraw();
                g.target->SetTransform(D2D1::Matrix3x2F::Identity());
                g.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                g.target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
                g.target->DrawText(run.text.c_str(), (UINT32)run.text.size(), format,
                                   D2D1::RectF(0, 0, (float)w, (float)h), g.white);
                g.target->EndDraw();
                BlendMask(dst, stride, width, height, run.x, run.y,
                          static_cast<const BYTE*>(bits), w, h, run.argb);
            }
        }

        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }
}

} // namespace dwtext
} // namespace ch
