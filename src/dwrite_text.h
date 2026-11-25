#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ch {
namespace dwtext {

// DirectWrite text for the taskbar pill. GDI+ DrawString uses GDI+ glyph rendering, which looks
// soft/hinted at 12 px; DirectWrite uses the same grayscale rasterizer as WPF, without pulling in
// .NET or WPF. The panel keeps GDI+ for its bundled Inter faces.
enum class Face {
    Ui,     // Segoe UI semibold (the taskbar font)
    Glyph,  // Segoe MDL2 Assets
};

struct Run {
    std::wstring text;
    float x = 0, y = 0, w = 0, h = 0;
    int align = 0;   // 0 leading, 1 center, 2 trailing
    Face face = Face::Ui;
    float px = 12;
    int weight = 600;
    uint32_t argb = 0xFFFFFFFF;
};

bool Init();
void Shutdown();
bool Available();

// Blends the runs into a premultiplied 32-bit BGRA surface (what Canvas presents).
void DrawRuns(void* premultipliedBits, int stride, int width, int height,
              const std::vector<Run>& runs);

} // namespace dwtext
} // namespace ch
