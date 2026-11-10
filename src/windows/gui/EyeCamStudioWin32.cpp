#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WindowsDirectShowSink.hpp"
#include "SharedMemoryProtocol.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

namespace {

// ============================================================================
// Кольорова палітра Windows 11 Dark Studio (Fluent / OLED Dark)
// ============================================================================
D2D1_COLOR_F HexColor(uint32_t rgb, float alpha = 1.0f) {
    return D2D1::ColorF(
        static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgb & 0xFF) / 255.0f,
        alpha
    );
}

const D2D1_COLOR_F COL_BG_MAIN       = HexColor(0x0B0E14);
const D2D1_COLOR_F COL_BG_PANEL      = HexColor(0x131822);
const D2D1_COLOR_F COL_BG_CARD       = HexColor(0x1A202C);
const D2D1_COLOR_F COL_BG_CARD_HOVER = HexColor(0x232B3A);
const D2D1_COLOR_F COL_BORDER        = HexColor(0x283142);
const D2D1_COLOR_F COL_EMERALD       = HexColor(0x00E676);
const D2D1_COLOR_F COL_CYAN          = HexColor(0x00E5FF);
const D2D1_COLOR_F COL_AMBER         = HexColor(0xFFB300);
const D2D1_COLOR_F COL_CRIMSON       = HexColor(0xFF1744);
const D2D1_COLOR_F COL_TEXT_PRIMARY  = HexColor(0xF0F4F8);
const D2D1_COLOR_F COL_TEXT_SECOND   = HexColor(0x94A3B8);
const D2D1_COLOR_F COL_TEXT_DIM      = HexColor(0x64748B);

struct UiRect {
    float left = 0;
    float top = 0;
    float right = 0;
    float bottom = 0;

    bool Contains(float x, float y) const {
        return x >= left && x <= right && y >= top && y <= bottom;
    }
    D2D1_RECT_F ToD2D() const {
        return D2D1::RectF(left, top, right, bottom;
    }
    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
};
