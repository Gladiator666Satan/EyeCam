#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <ole2.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <winhttp.h>

#ifdef DrawText
#undef DrawText
#endif

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dxgiformat.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "WindowsDirectShowSink.hpp"
#include "SharedMemoryProtocol.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comdlg32.lib")

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

static constexpr const char* kEyeCamVersionStr = "1.4.0";
static constexpr const wchar_t* kEyeCamVersionWide = L"1.4.0";

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
const D2D1_COLOR_F COL_BG_DROPDOWN   = HexColor(0x0F141D);
const D2D1_COLOR_F COL_BORDER        = HexColor(0x283142);
const D2D1_COLOR_F COL_EMERALD       = HexColor(0x00E676);
const D2D1_COLOR_F COL_CYAN          = HexColor(0x00E5FF);
const D2D1_COLOR_F COL_AMBER         = HexColor(0xFFB300);
const D2D1_COLOR_F COL_CRIMSON       = HexColor(0xFF1744);
const D2D1_COLOR_F COL_PATREON       = HexColor(0xFF424D);
const D2D1_COLOR_F COL_BMAC          = HexColor(0xFFDD00);
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
        return D2D1::RectF(left, top, right, bottom);
    }
    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
};

enum class TransportTab {
    WIFI_LAN = 0,
    USB_ADB  = 1
};

enum class ModalOverlay {
    NONE = 0,
    SUPPORT_AUTHOR = 1,
    GITHUB_UPDATE = 2
};

struct UsbDeviceEntry {
    std::string serial;
    std::wstring displayName;
    bool authorized = true;
};

// ============================================================================
// Парсер та процесор 3D/1D кольорових профілів формату Adobe .cube (32x32x32)
// ============================================================================
class CubeLut3D {
public:
    bool LoadFromFile(const std::wstring& filePath, std::wstring& outName) {
        std::ifstream file(filePath);
        if (!file.is_open()) return false;

        int lut3dSize = 0;
        int lut1dSize = 0;
        float domainMin[3] = {0.0f, 0.0f, 0.0f};
        float domainMax[3] = {1.0f, 1.0f, 1.0f};
        std::vector<float> values;
        values.reserve(33 * 33 * 33 * 3);

        std::string line;
        while (std::getline(file, line)) {
            size_t hashPos = line.find('#');
            if (hashPos != std::string::npos) {
                line.resize(hashPos);
            }
            size_t first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) continue;
            size_t last = line.find_last_not_of(" \t\r\n");
            std::string trimmed = line.substr(first, last - first + 1);
            if (trimmed.empty()) continue;

            std::istringstream iss(trimmed);
            std::string token;
            iss >> token;
            for (char& c : token) {
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            }

            if (token == "TITLE") {
                continue;
            } else if (token == "LUT_3D_SIZE") {
                iss >> lut3dSize;
            } else if (token == "LUT_1D_SIZE") {
                iss >> lut1dSize;
            } else if (token == "DOMAIN_MIN") {
                iss >> domainMin[0] >> domainMin[1] >> domainMin[2];
            } else if (token == "DOMAIN_MAX") {
                iss >> domainMax[0] >> domainMax[1] >> domainMax[2];
            } else if (token == "LUT_1D_INPUT_RANGE" || token == "LUT_3D_INPUT_RANGE") {
                iss >> domainMin[0] >> domainMax[0];
                domainMin[1] = domainMin[2] = domainMin[0];
                domainMax[1] = domainMax[2] = domainMax[0];
            } else if ((token[0] >= '0' && token[0] <= '9') || token[0] == '-' || token[0] == '+' || token[0] == '.') {
                float r = static_cast<float>(std::atof(token.c_str()));
                float g = 0.0f, b = 0.0f;
                if (iss >> g >> b) {
                    values.push_back(r);
                    values.push_back(g);
                    values.push_back(b);
                }
            }
        }

        if (lut3dSize < 2 && lut1dSize < 2) {
            int triplets = static_cast<int>(values.size() / 3);
            int cbrtVal = static_cast<int>(std::round(std::cbrt(static_cast<double>(triplets))));
            if (cbrtVal >= 2 && cbrtVal * cbrtVal * cbrtVal == triplets) {
                lut3dSize = cbrtVal;
            } else if (triplets >= 16) {
                lut1dSize = triplets;
            } else {
                return false;
            }
        }

        const float spanR = (std::max)(1e-5f, domainMax[0] - domainMin[0]);
        const float spanG = (std::max)(1e-5f, domainMax[1] - domainMin[1]);
        const float spanB = (std::max)(1e-5f, domainMax[2] - domainMin[2]);

        std::array<uint8_t, 32 * 32 * 32 * 3> baked{};

        if (lut3dSize >= 2) {
            size_t expected = static_cast<size_t>(lut3dSize) * lut3dSize * lut3dSize * 3;
            if (values.size() < expected) return false;
            const int n = lut3dSize;
            const int maxIdx = n - 1;

            auto sampleSrc = [&](int rI, int gI, int bI, int ch) -> float {
                int rc = std::clamp(rI, 0, maxIdx);
                int gc = std::clamp(gI, 0, maxIdx);
                int bc = std::clamp(bI, 0, maxIdx);
                size_t idx = (static_cast<size_t>(bc) * n * n + static_cast<size_t>(gc) * n + static_cast<size_t>(rc)) * 3 + ch;
                return values[idx];
            };

            for (int bOut = 0; bOut < 32; ++bOut) {
                float uB = std::clamp(((bOut / 31.0f) - domainMin[2]) / spanB, 0.0f, 1.0f) * maxIdx;
                int b0 = static_cast<int>(uB);
                int b1 = (std::min)(b0 + 1, maxIdx);
                float fb = uB - b0;

                for (int gOut = 0; gOut < 32; ++gOut) {
                    float uG = std::clamp(((gOut / 31.0f) - domainMin[1]) / spanG, 0.0f, 1.0f) * maxIdx;
                    int g0 = static_cast<int>(uG);
                    int g1 = (std::min)(g0 + 1, maxIdx);
                    float fg = uG - g0;

                    for (int rOut = 0; rOut < 32; ++rOut) {
                        float uR = std::clamp(((rOut / 31.0f) - domainMin[0]) / spanR, 0.0f, 1.0f) * maxIdx;
                        int r0 = static_cast<int>(uR);
                        int r1 = (std::min)(r0 + 1, maxIdx);
                        float fr = uR - r0;

                        size_t dstIdx = ((bOut * 32 + gOut) * 32 + rOut) * 3;
                        for (int ch = 0; ch < 3; ++ch) {
                            float c000 = sampleSrc(r0, g0, b0, ch);
                            float c100 = sampleSrc(r1, g0, b0, ch);
                            float c010 = sampleSrc(r0, g1, b0, ch);
                            float c110 = sampleSrc(r1, g1, b0, ch);
                            float c001 = sampleSrc(r0, g0, b1, ch);
                            float c101 = sampleSrc(r1, g0, b1, ch);
                            float c011 = sampleSrc(r0, g1, b1, ch);
                            float c111 = sampleSrc(r1, g1, b1, ch);

                            float c00 = c000 + (c100 - c000) * fr;
                            float c10 = c010 + (c110 - c010) * fr;
                            float c01 = c001 + (c101 - c001) * fr;
                            float c11 = c011 + (c111 - c011) * fr;
                            float c0 = c00 + (c10 - c00) * fg;
                            float c1 = c01 + (c11 - c01) * fg;
                            float c = c0 + (c1 - c0) * fb;

                            baked[dstIdx + ch] = static_cast<uint8_t>(std::clamp(static_cast<int>(c * 255.0f + 0.5f), 0, 255));
                        }
                    }
                }
            }
        } else {
            size_t expected = static_cast<size_t>(lut1dSize) * 3;
            if (values.size() < expected) return false;
            const int maxIdx = lut1dSize - 1;
            auto sample1D = [&](float u, int ch) -> uint8_t {
                float pos = std::clamp(u, 0.0f, 1.0f) * maxIdx;
                int i0 = static_cast<int>(pos);
                int i1 = (std::min)(i0 + 1, maxIdx);
                float f = pos - i0;
                float v0 = values[static_cast<size_t>(i0) * 3 + ch];
                float v1 = values[static_cast<size_t>(i1) * 3 + ch];
                float v = v0 + (v1 - v0) * f;
                return static_cast<uint8_t>(std::clamp(static_cast<int>(v * 255.0f + 0.5f), 0, 255));
            };

            for (int bOut = 0; bOut < 32; ++bOut) {
                uint8_t bVal = sample1D(bOut / 31.0f, 2);
                for (int gOut = 0; gOut < 32; ++gOut) {
                    uint8_t gVal = sample1D(gOut / 31.0f, 1);
                    for (int rOut = 0; rOut < 32; ++rOut) {
                        uint8_t rVal = sample1D(rOut / 31.0f, 0);
                        size_t dstIdx = ((bOut * 32 + gOut) * 32 + rOut) * 3;
                        baked[dstIdx + 0] = rVal;
                        baked[dstIdx + 1] = gVal;
                        baked[dstIdx + 2] = bVal;
                    }
                }
            }
        }

        size_t slashPos = filePath.find_last_of(L"\\/");
        outName = (slashPos != std::wstring::npos) ? filePath.substr(slashPos + 1) : filePath;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_table = baked;
        m_loaded.store(true, std::memory_order_release);
        return true;
    }

    void Clear() {
        m_loaded.store(false, std::memory_order_release);
    }

    bool IsLoaded() const {
        return m_loaded.load(std::memory_order_acquire);
    }

    inline void ApplyRgb(int& r, int& g, int& b) const {
        int rIdx = std::clamp(r, 0, 255) >> 3;
        int gIdx = std::clamp(g, 0, 255) >> 3;
        int bIdx = std::clamp(b, 0, 255) >> 3;
        size_t idx = static_cast<size_t>((bIdx << 10) | (gIdx << 5) | rIdx) * 3;
        r = m_table[idx + 0];
        g = m_table[idx + 1];
        b = m_table[idx + 2];
    }

private:
    std::atomic<bool> m_loaded{false};
    mutable std::mutex m_mutex;
    std::array<uint8_t, 32 * 32 * 32 * 3> m_table{};
};

struct StudioState {
    // Мова інтерфейсу (за замовчуванням English (EN) при першому встановленні, перемикається на UA)
    bool isUkrainian = false;

    // Підключення та транспорт (Wi-Fi LAN або USB ADB)
    TransportTab transport = TransportTab::USB_ADB;
    std::string deviceIp = "192.168.1.105";
    bool ipInputFocused = false;
    bool isStreaming = true;
    bool isDriverRegistered = false;
    std::atomic<bool> isPhoneConnected{false};

    // Випадаючий список USB пристроїв (ADB)
    std::vector<UsbDeviceEntry> usbDevices;
    std::string selectedUsbSerial;
    int selectedUsbIdx = 0;
    bool usbDropdownOpen = false;

    // Профіль підключеного смартфона
    std::wstring connectedPhoneName = L"Waiting for smartphone...";
    float phoneMinZoom = 1.0f;
    float phoneMaxZoom = 8.0f;
    bool phoneSupports4K = true;
    bool phoneSupports2K = true;
    bool phoneSupports60Fps = true;
    bool phoneIsLowEnd = false;

    // Доступність окремих фізичних модулів: 0:UW (0.5x), 1:Main (1x), 2:Tele (2x/3x), 3:Tele5x/Macro, 4:Front
    bool moduleAvailable[5] = {false, true, false, false, true};
    int activeModuleIdx = 1; // 1 = Main (1x) за замовчуванням
    float intraModuleZoom = 1.0f;
    float maxIntraModuleZoom = 8.0f;
    float zoomSliderNorm = 0.0f; // 0.0 .. 1.0 в межах поточного модуля (1.0x .. maxIntraModuleZoom)

    // Ліхтарик (Torch) та блокування експозиції (AE Lock)
    bool torchOn = false;
    bool torchAvailable = true;
    bool aeLocked = false;

    int streamRotationDeg = -1; // -1 = Авто, 0, 90, 180, 270
    float phoneThermalC = 0.0f;
    int phoneBatteryPct = 0;
    int phoneMeasuredFps = 0;
    int phoneMeasuredBitrateKbps = 0;

    // Параметри відеопотоку
    int resPresetIdx = 2; // 0: 4K UHD (3840x2160), 1: 2K QHD (2560x1440), 2: 1080p, 3: 720p
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;

    bool mirrorHorizontal = false;
    bool showGrid = false;

    // Кожен повзунок за замовчуванням АВТОМАТИЧНИЙ, поки користувач не почне крутити його вручну.
    // Тоді з'являється замочок 🔒, натискання на який повертає конкретний повзунок у режим AUTO.
    bool sliderManual[4] = {false, false, false, false}; // 0:ISO, 1:Shutter, 2:Focus, 3:WB
    float isoNorm = 0.20f;     // 100 .. 3200
    float shutterNorm = 0.35f; // 1/30 .. 1/4000
    float focusNorm = 0.0f;    // 0.0 .. 1.0 (0 .. 10 dpt)
    float wbNorm = 0.48f;      // 2800K .. 8500K

    // Візуальний відгук кнопок Автофокусу та Авторежиму
    uint32_t afPulseUntilTick = 0;
    uint32_t autoPulseUntilTick = 0;
    float afTargetNormX = 0.5f;
    float afTargetNormY = 0.5f;

    // Кольоровий профіль .cube 3D LUT
    std::wstring activeLutName = L"";

    // Модальні вікна (Підтримка автора та GitHub / Перевірка оновлень)
    ModalOverlay activeModal = ModalOverlay::NONE;
    std::atomic<bool> updateCheckInProgress{false};
    std::atomic<bool> updateAvailable{false};
    std::wstring latestGithubTag = L"1.4.0";
    std::wstring updateStatusText = L"Натисніть «Перевірити оновлення» для запиту до GitHub Releases";
    std::wstring releaseHtmlUrl = L"https://github.com/Gladiator666Satan/EyeCam/releases/latest";

    // Телеметрія реального часу
    std::atomic<uint64_t> rxFrames{0};
    std::atomic<uint64_t> rxBytes{0};
    std::atomic<uint32_t> measuredBitrateKbps{0};
    std::atomic<uint32_t> measuredFps{0};
    std::atomic<uint64_t> shmFramesWritten{0};
};

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (sz <= 0) return L"";
    std::wstring res(sz, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), res.data(), sz);
    return res;
}

bool IsVersionNewer(const std::string& latestTag, const std::string& currentVer) {
    auto parseParts = [](const std::string& s) {
        std::vector<int> parts;
        int cur = 0;
        bool inNum = false;
        for (char c : s) {
            if (c >= '0' && c <= '9') {
                cur = cur * 10 + (c - '0');
                inNum = true;
            } else if (inNum) {
                parts.push_back(cur);
                cur = 0;
                inNum = false;
            }
        }
        if (inNum) parts.push_back(cur);
        return parts;
    };
    auto a = parseParts(latestTag);
    auto b = parseParts(currentVer);
    size_t n = (std::max)(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        int va = (i < a.size()) ? a[i] : 0;
        int vb = (i < b.size()) ? b[i] : 0;
        if (va > vb) return true;
        if (va < vb) return false;
    }
    return false;
}

// Пошук виконуваного файлу adb.exe (вбудованого в інсталятор або системного)
std::wstring ResolveAdbExecutablePath() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dirPath(exePath);
    size_t pos = dirPath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        dirPath = dirPath.substr(0, pos);
    }

    std::wstring bundled1 = dirPath + L"\\adb\\adb.exe";
    if (GetFileAttributesW(bundled1.c_str()) != INVALID_FILE_ATTRIBUTES) return bundled1;

    std::wstring bundled2 = dirPath + L"\\adb.exe";
    if (GetFileAttributesW(bundled2.c_str()) != INVALID_FILE_ATTRIBUTES) return bundled2;

    wchar_t localAppData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        std::wstring sdkAdb = std::wstring(localAppData) + L"\\Android\\Sdk\\platform-tools\\adb.exe";
        if (GetFileAttributesW(sdkAdb.c_str()) != INVALID_FILE_ATTRIBUTES) return sdkAdb;
    }

    return L"adb.exe";
}

// Тихий запуск консольної команди (без миготіння чорного вікна CMD) з читанням stdout
std::string RunHiddenCommandOutput(const std::wstring& cmdLine) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr;
    HANDLE hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "";
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdLine;
    BOOL ok = CreateProcessW(
        nullptr,
        mutableCmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );
    CloseHandle(hWrite);

    if (!ok) {
        CloseHandle(hRead);
        return "";
    }

    std::string output;
    char buf[1024];
    DWORD bytesRead = 0;
    while (ReadFile(hRead, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
        output.append(buf, bytesRead);
    }

    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return output;
}

// Сканування підключених по USB смартфонів через `adb devices -l`
std::vector<UsbDeviceEntry> ScanConnectedUsbDevices(const std::string& preferredSerial = "") {
    std::vector<UsbDeviceEntry> list;
    std::wstring adbPath = ResolveAdbExecutablePath();
    std::string out = RunHiddenCommandOutput(L"\"" + adbPath + L"\" devices -l");
    if (out.empty()) return list;

    size_t pos = 0;
    while (pos < out.size()) {
        size_t end = out.find('\n', pos);
        if (end == std::string::npos) end = out.size();
        std::string line = out.substr(pos, end - pos);
        pos = end + 1;

        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.find("List of devices") != std::string::npos || line[0] == '*') continue;

        size_t spacePos = line.find_first_of(" \t");
        if (spacePos == std::string::npos) continue;
        std::string serial = line.substr(0, spacePos);
        std::string rest = line.substr(spacePos);

        if (serial.find(':') != std::string::npos) continue;

        bool isUnauthorized = (rest.find("unauthorized") != std::string::npos);
        bool isOffline = (rest.find("offline") != std::string::npos);
        if (isOffline) continue;

        std::string modelName = "Android USB Smartphone";
        size_t mPos = rest.find("model:");
        if (mPos != std::string::npos) {
            size_t mStart = mPos + 6;
            size_t mEnd = rest.find_first_of(" \t\r\n", mStart);
            modelName = rest.substr(mStart, (mEnd == std::string::npos) ? std::string::npos : (mEnd - mStart));
            std::replace(modelName.begin(), modelName.end(), '_', ' ');
        }

        UsbDeviceEntry entry;
        entry.serial = serial;
        entry.authorized = !isUnauthorized;
        std::wstring wModel = Utf8ToWide(modelName);
        std::wstring wSerial = Utf8ToWide(serial);
        if (isUnauthorized) {
            entry.displayName = wModel + L" (" + wSerial + L") — Підтвердіть USB на телефоні!";
        } else {
            entry.displayName = wModel + L"  [" + wSerial + L"]";
        }
        list.push_back(entry);
    }

    const UsbDeviceEntry* targetDev = nullptr;
    if (!preferredSerial.empty()) {
        for (const auto& dev : list) {
            if (dev.authorized && dev.serial == preferredSerial) {
                targetDev = &dev;
                break;
            }
        }
    }
    if (!targetDev) {
        for (const auto& dev : list) {
            if (dev.authorized) {
                targetDev = &dev;
                break;
            }
        }
    }
    if (targetDev) {
        std::wstring wSer = Utf8ToWide(targetDev->serial);
        RunHiddenCommandOutput(L"\"" + adbPath + L"\" -s " + wSer + L" forward tcp:8554 tcp:8554");
    }
    return list;
}

// Перевірка реєстрації DirectShow фільтра EyeCam у реєстрі Windows
bool CheckDirectShowDriverRegistered() {
    HKEY hKey = nullptr;
    const wchar_t* subKey = L"CLSID\\{5C9D017A-C081-4FE7-99E4-500D06474F29}\\InprocServer32";
    LONG res = RegOpenKeyExW(HKEY_CLASSES_ROOT, subKey, 0, KEY_READ, &hKey);
    if (res == ERROR_SUCCESS && hKey != nullptr) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

// Реєстрація DirectShow фільтра EyeCamFilter.dll з підвищенням прав (UAC)
bool RegisterDirectShowDriver(HWND hwnd) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dirPath(exePath);
    size_t pos = dirPath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        dirPath = dirPath.substr(0, pos);
    }
    std::wstring dllPath = dirPath + L"\\EyeCamFilter.dll";

    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hwnd,
            L"Файл EyeCamFilter.dll не знайдено у директорії програми.\n"
            L"Переконайтеся, що EyeCamFilter.dll знаходиться поруч із EyeCamStudio.exe.",
            L"EyeCam Studio — Помилка драйвера",
            MB_ICONWARNING | MB_OK);
        return false;
    }

    std::wstring params = L"/s \"" + dllPath + L"\"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = L"regsvr32.exe";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;

    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 5000);
            CloseHandle(sei.hProcess);
        }
        return CheckDirectShowDriverRegistered();
    }
    return false;
}

bool ExtractJsonString(const std::string& json, const char* key, std::string& outVal) {
    std::string pattern = std::string("\"") + key + "\":\"";
    size_t p = json.find(pattern);
    if (p == std::string::npos) return false;
    size_t start = p + pattern.size();
    size_t end = json.find('"', start);
    if (end == std::string::npos) return false;
    outVal = json.substr(start, end - start);
    return true;
}

bool ExtractJsonFloat(const std::string& json, const char* key, float& outVal) {
    std::string pattern = std::string("\"") + key + "\":";
    size_t p = json.find(pattern);
    if (p == std::string::npos) return false;
    size_t start = p + pattern.size();
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) ++start;
    if (start >= json.size()) return false;
    outVal = static_cast<float>(std::atof(json.c_str() + start));
    return true;
}

bool ExtractJsonInt(const std::string& json, const char* key, int& outVal) {
    std::string pattern = std::string("\"") + key + "\":";
    size_t p = json.find(pattern);
    if (p == std::string::npos) return false;
    size_t start = p + pattern.size();
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) ++start;
    if (start >= json.size()) return false;
    outVal = std::atoi(json.c_str() + start);
    return true;
}

bool ExtractJsonBool(const std::string& json, const char* key, bool& outVal) {
    std::string pattern = std::string("\"") + key + "\":";
    size_t p = json.find(pattern);
    if (p == std::string::npos) return false;
    size_t start = p + pattern.size();
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) ++start;
    if (json.compare(start, 4, "true") == 0) { outVal = true; return true; }
    if (json.compare(start, 5, "false") == 0) { outVal = false; return true; }
    return false;
}

// Запит до GitHub API (WinHTTP) для перевірки наявності нової версії у релізах
bool QueryGithubLatestRelease(std::string& outTagName, std::string& outHtmlUrl) {
    HINTERNET hSession = WinHttpOpen(
        L"EyeCamStudio-Updater/1.4",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!hSession) return false;

    WinHttpSetTimeouts(hSession, 4000, 4000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        L"/repos/Gladiator666Satan/EyeCam/releases/latest",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    const wchar_t* headers = L"Accept: application/vnd.github+json\r\nUser-Agent: EyeCamStudio/1.4.0\r\n";
    BOOL sent = WinHttpSendRequest(
        hRequest,
        headers,
        static_cast<DWORD>(-1L),
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0
    );

    std::string body;
    if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX
        );
        if (statusCode == 200) {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
                std::vector<char> chunk(avail);
                DWORD readBytes = 0;
                if (WinHttpReadData(hRequest, chunk.data(), avail, &readBytes) && readBytes > 0) {
                    body.append(chunk.data(), readBytes);
                } else {
                    break;
                }
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (body.empty()) return false;
    if (!ExtractJsonString(body, "tag_name", outTagName) || outTagName.empty()) return false;
    if (!ExtractJsonString(body, "html_url", outHtmlUrl) || outHtmlUrl.empty()) {
        outHtmlUrl = "https://github.com/Gladiator666Satan/EyeCam/releases/latest";
    }
    return true;
}

bool IsH264KeyFrameAnnexB(const uint8_t* data, size_t len) {
    if (!data || len < 5) return false;
    for (size_t i = 0; i + 4 < len; ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            size_t hdr = 0;
            if (data[i + 2] == 0x01) {
                hdr = i + 3;
            } else if (data[i + 2] == 0x00 && i + 4 < len && data[i + 3] == 0x01) {
                hdr = i + 4;
            }
            if (hdr > 0 && hdr < len) {
                uint8_t nalType = data[hdr] & 0x1F;
                if (nalType == 5) return true;
                i = hdr;
            }
        }
    }
    return false;
}

struct OutboundControlPacket {
    uint16_t msgType = 1;
    std::string payload;
    std::string coalesceKey;
};

struct IncomingVideoPacket {
    enum class Kind { H264, JPEG, FLUSH };
    Kind kind = Kind::H264;
    bool isKeyFrame = false;
    std::vector<uint8_t> data;
};

// ============================================================================
// Апаратний / системний декодер H.264 Annex-B на базі Windows Media Foundation
// ============================================================================
class MfH264Decoder {
public:
    ~MfH264Decoder() {
        Shutdown();
    }

    bool Initialize(UINT32 hintW = 1920, UINT32 hintH = 1080) {
        Shutdown();

        HRESULT hr = CoCreateInstance(
            CLSID_CMSH264DecoderMFT,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&m_decoder)
        );
        if (FAILED(hr) || !m_decoder) {
            return false;
        }

        ICodecAPI* pCodecApi = nullptr;
        if (SUCCEEDED(m_decoder->QueryInterface(IID_PPV_ARGS(&pCodecApi))) && pCodecApi) {
            static constexpr GUID kAvLowLatencyMode = {
                0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}
            };
            VARIANT var{};
            VariantInit(&var);
            var.vt = VT_UI4;
            var.ulVal = 1;
            if (FAILED(pCodecApi->SetValue(&kAvLowLatencyMode, &var))) {
                VariantClear(&var);
                var.vt = VT_BOOL;
                var.boolVal = VARIANT_TRUE;
                pCodecApi->SetValue(&kAvLowLatencyMode, &var);
            }
            VariantClear(&var);
            pCodecApi->Release();
        }

        IMFMediaType* pInType = nullptr;
        hr = MFCreateMediaType(&pInType);
        if (FAILED(hr) || !pInType) {
            Shutdown();
            return false;
        }
        pInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pInType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        pInType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(pInType, MF_MT_FRAME_SIZE, (std::max)(16u, hintW), (std::max)(16u, hintH));
        hr = m_decoder->SetInputType(0, pInType, 0);
        pInType->Release();
        if (FAILED(hr)) {
            Shutdown();
            return false;
        }

        if (!ConfigureOutputType()) {
            Shutdown();
            return false;
        }

        m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        m_sampleTime100ns = 0;
        m_initialized = true;
        return true;
    }

    void Flush() {
        if (m_decoder) {
            m_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
    }

    void Shutdown() {
        ReleaseReusableSample();
        if (m_decoder) {
            m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            m_decoder->Release();
            m_decoder = nullptr;
        }
        m_initialized = false;
        m_outInfoValid = false;
        m_mftProvidesSamples = false;
    }

    template <typename OnNv12Frame>
    bool DecodeAccessUnit(const uint8_t* data, size_t len, OnNv12Frame&& onFrame) {
        if (!data || len == 0) return false;
        if (!m_initialized && !Initialize()) return false;

        IMFMediaBuffer* pInBuf = nullptr;
        HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(len), &pInBuf);
        if (FAILED(hr) || !pInBuf) return false;

        BYTE* pDst = nullptr;
        if (SUCCEEDED(pInBuf->Lock(&pDst, nullptr, nullptr)) && pDst) {
            std::memcpy(pDst, data, len);
            pInBuf->Unlock();
            pInBuf->SetCurrentLength(static_cast<DWORD>(len));
        } else {
            pInBuf->Release();
            return false;
        }

        IMFSample* pInSample = nullptr;
        hr = MFCreateSample(&pInSample);
        if (FAILED(hr) || !pInSample) {
            pInBuf->Release();
            return false;
        }
        pInSample->AddBuffer(pInBuf);
        pInSample->SetSampleTime(m_sampleTime100ns);
        pInSample->SetSampleDuration(166667);
        m_sampleTime100ns += 166667;
        pInBuf->Release();

        bool emitted = false;
        hr = m_decoder->ProcessInput(0, pInSample, 0);
        if (hr == MF_E_NOTACCEPTING) {
            emitted = DrainOutputs(onFrame) || emitted;
            hr = m_decoder->ProcessInput(0, pInSample, 0);
        }
        pInSample->Release();

        if (FAILED(hr)) {
            return emitted;
        }
        return DrainOutputs(onFrame) || emitted;
    }

private:
    void ReleaseReusableSample() {
        if (m_reusableOutSample) {
            m_reusableOutSample->Release();
            m_reusableOutSample = nullptr;
        }
        m_reusableOutBufBytes = 0;
    }

    bool ConfigureOutputType() {
        if (!m_decoder) return false;
        for (DWORD idx = 0;; ++idx) {
            IMFMediaType* pOutType = nullptr;
            HRESULT hr = m_decoder->GetOutputAvailableType(0, idx, &pOutType);
            if (FAILED(hr) || !pOutType) break;

            GUID subtype{};
            if (SUCCEEDED(pOutType->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) {
                hr = m_decoder->SetOutputType(0, pOutType, 0);
                if (SUCCEEDED(hr)) {
                    UINT32 cw = 1920, ch = 1080;
                    if (SUCCEEDED(MFGetAttributeSize(pOutType, MF_MT_FRAME_SIZE, &cw, &ch)) && cw > 0 && ch > 0) {
                        m_codedW = cw;
                        m_codedH = ch;
                    }
                    UINT32 defStrideU = m_codedW;
                    if (SUCCEEDED(pOutType->GetUINT32(MF_MT_DEFAULT_STRIDE, &defStrideU)) && defStrideU != 0) {
                        m_defaultStride = static_cast<LONG>(static_cast<INT32>(defStrideU));
                    } else {
                        m_defaultStride = static_cast<LONG>(m_codedW);
                    }

                    m_visX = 0;
                    m_visY = 0;
                    m_visW = m_codedW;
                    m_visH = m_codedH;

                    MFVideoArea area{};
                    UINT32 blobSz = 0;
                    if (SUCCEEDED(pOutType->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                                    reinterpret_cast<UINT8*>(&area),
                                                    sizeof(area), &blobSz)) &&
                        blobSz == sizeof(MFVideoArea) && area.Area.cx > 0 && area.Area.cy > 0) {
                        m_visX = (area.OffsetX.value >= 0) ? static_cast<UINT32>(area.OffsetX.value) : 0u;
                        m_visY = (area.OffsetY.value >= 0) ? static_cast<UINT32>(area.OffsetY.value) : 0u;
                        m_visW = static_cast<UINT32>(area.Area.cx);
                        m_visH = static_cast<UINT32>(area.Area.cy);
                    }

                    m_outInfoValid = false;
                    ReleaseReusableSample();
                    pOutType->Release();
                    return true;
                }
            }
            pOutType->Release();
        }
        return false;
    }

    bool EnsureReusableSample(DWORD requiredBytes) {
        if (m_reusableOutSample && m_reusableOutBufBytes >= requiredBytes) {
            IMFMediaBuffer* pBuf = nullptr;
            if (SUCCEEDED(m_reusableOutSample->GetBufferByIndex(0, &pBuf)) && pBuf) {
                pBuf->SetCurrentLength(0);
                pBuf->Release();
                return true;
            }
        }
        ReleaseReusableSample();

        IMFMediaBuffer* pBuf = nullptr;
        if (FAILED(MFCreateMemoryBuffer(requiredBytes, &pBuf)) || !pBuf) return false;
        IMFSample* pSample = nullptr;
        if (FAILED(MFCreateSample(&pSample)) || !pSample) {
            pBuf->Release();
            return false;
        }
        pSample->AddBuffer(pBuf);
        pBuf->Release();
        m_reusableOutSample = pSample;
        m_reusableOutBufBytes = requiredBytes;
        return true;
    }

    template <typename OnNv12Frame>
    bool DrainOutputs(OnNv12Frame&& onFrame) {
        bool emitted = false;
        for (int iter = 0; iter < 16; ++iter) {
            if (!m_outInfoValid) {
                MFT_OUTPUT_STREAM_INFO outInfo{};
                if (FAILED(m_decoder->GetOutputStreamInfo(0, &outInfo))) break;
                m_mftProvidesSamples = (outInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
                DWORD nv12Min = static_cast<DWORD>(m_codedW * m_codedH * 3 / 2 + 4096);
                m_requiredOutBytes = (std::max)(outInfo.cbSize, nv12Min);
                m_outInfoValid = true;
            }

            MFT_OUTPUT_DATA_BUFFER outData{};
            outData.dwStreamID = 0;
            if (!m_mftProvidesSamples) {
                if (!EnsureReusableSample(m_requiredOutBytes)) break;
                outData.pSample = m_reusableOutSample;
            }

            DWORD status = 0;
            HRESULT hr = m_decoder->ProcessOutput(0, 1, &outData, &status);
            if (outData.pEvents) {
                outData.pEvents->Release();
                outData.pEvents = nullptr;
            }

            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (m_mftProvidesSamples && outData.pSample) {
                    outData.pSample->Release();
                }
                if (!ConfigureOutputType()) break;
                continue;
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(hr)) {
                if (m_mftProvidesSamples && outData.pSample) {
                    outData.pSample->Release();
                }
                break;
            }

            if (outData.pSample) {
                IMFMediaBuffer* pOutBuf = nullptr;
                if (SUCCEEDED(outData.pSample->GetBufferByIndex(0, &pOutBuf)) && pOutBuf) {
                    UINT32 visX = (std::min)(m_visX, m_codedW > 0 ? m_codedW - 1 : 0u);
                    UINT32 visY = (std::min)(m_visY, m_codedH > 0 ? m_codedH - 1 : 0u);
                    UINT32 visW = (std::min)(m_visW, m_codedW - visX);
                    UINT32 visH = (std::min)(m_visH, m_codedH - visY);

                    IMF2DBuffer* p2D = nullptr;
                    if (SUCCEEDED(pOutBuf->QueryInterface(IID_PPV_ARGS(&p2D))) && p2D) {
                        BYTE* pScan0 = nullptr;
                        LONG stride = 0;
                        if (SUCCEEDED(p2D->Lock2D(&pScan0, &stride)) && pScan0 && visW >= 2 && visH >= 2) {
                            onFrame(pScan0, stride, m_codedW, m_codedH, visX, visY, visW, visH);
                            emitted = true;
                            p2D->Unlock2D();
                        }
                        p2D->Release();
                    } else {
                        BYTE* pData = nullptr;
                        DWORD cbLen = 0;
                        if (SUCCEEDED(pOutBuf->Lock(&pData, nullptr, &cbLen)) && pData && visW >= 2 && visH >= 2) {
                            LONG stride = (m_defaultStride > 0) ? m_defaultStride : static_cast<LONG>(m_codedW);
                            size_t needed = static_cast<size_t>(stride) * m_codedH * 3 / 2;
                            if (cbLen >= needed) {
                                onFrame(pData, stride, m_codedW, m_codedH, visX, visY, visW, visH);
                                emitted = true;
                            }
                            pOutBuf->Unlock();
                        }
                    }
                    pOutBuf->Release();
                }
                if (m_mftProvidesSamples) {
                    outData.pSample->Release();
                }
            }
        }
        return emitted;
    }

    IMFTransform* m_decoder = nullptr;
    IMFSample* m_reusableOutSample = nullptr;
    DWORD m_reusableOutBufBytes = 0;
    DWORD m_requiredOutBytes = 0;
    bool m_mftProvidesSamples = false;
    bool m_outInfoValid = false;
    bool m_initialized = false;
    UINT32 m_codedW = 1920;
    UINT32 m_codedH = 1080;
    LONG m_defaultStride = 1920;
    UINT32 m_visX = 0;
    UINT32 m_visY = 0;
    UINT32 m_visW = 1920;
    UINT32 m_visH = 1080;
    LONGLONG m_sampleTime100ns = 0;
};

} // namespace

// ============================================================================
// Головний клас вікна EyeCam Studio (Win11 Fluent Dark UI)
// ============================================================================
class EyeCamStudioWindow {
public:
    EyeCamStudioWindow() = default;
    ~EyeCamStudioWindow() {
        Shutdown();
    }

    bool Initialize(HINSTANCE hInstance, int nCmdShow) {
        WSADATA wsaData{};
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pD2DFactory);
        if (FAILED(hr)) return false;

        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&m_pDWriteFactory)
        );
        if (FAILED(hr)) return false;

        CreateTextFormats();

        // Завантаження уніфікованої іконки Cyber Eye Camera Lens (eyecam_pc.ico)
        HICON hAppIcon = LoadCustomStudioIcon(hInstance);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = EyeCamStudioWindow::WndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = hAppIcon;
        wc.hIconSm = hAppIcon;
        wc.lpszClassName = L"EyeCamStudioWin11Class";
        RegisterClassExW(&wc);

        m_hwnd = CreateWindowExW(
            0,
            wc.lpszClassName,
            L"EyeCam Studio v1.4.0 — Ultra-Low Latency 4K Virtual Webcam",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1290, 810,
            nullptr, nullptr, hInstance, this
        );
        if (!m_hwnd) return false;

        SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hAppIcon));
        SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hAppIcon));

        // Активація темної теми заголовка Windows 11 та заокруглених кутів
        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
        DWORD cornerPref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
        COLORREF captionCol = RGB(11, 14, 20);
        DwmSetWindowAttribute(m_hwnd, DWMWA_CAPTION_COLOR, &captionCol, sizeof(captionCol));

        m_state.isUkrainian = LoadSavedLanguageIsUa();
        m_state.isDriverRegistered = CheckDirectShowDriverRegistered();

        if (!m_state.isDriverRegistered) {
            wchar_t exePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring dirPath(exePath);
            size_t pos = dirPath.find_last_of(L"\\/");
            if (pos != std::wstring::npos) dirPath = dirPath.substr(0, pos);
            std::wstring dllPath = dirPath + L"\\EyeCamFilter.dll";
            HMODULE hFilter = LoadLibraryW(dllPath.c_str());
            if (hFilter) {
                using DllRegFn = HRESULT(STDAPICALLTYPE*)();
                auto fn = reinterpret_cast<DllRegFn>(GetProcAddress(hFilter, "DllRegisterServer"));
                if (fn && SUCCEEDED(fn())) {
                    m_state.isDriverRegistered = true;
                }
                FreeLibrary(hFilter);
            }
        }

        m_mfStarted = SUCCEEDED(MFStartup(MF_VERSION));

        // Початкове сканування USB ADB пристроїв та фонова перевірка оновлень на GitHub
        RefreshUsbDevicesAsync();
        CheckGithubUpdateAsync();

        // Запуск каналу спільної пам'яті DirectShow віртуальної камери
        {
            std::lock_guard<std::mutex> lock(m_sinkMutex);
            m_sink.Open(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2, m_state.fps);
        }

        m_running = true;
        m_sendThread = std::thread(&EyeCamStudioWindow::ControlSenderLoop, this);
        m_decodeThread = std::thread(&EyeCamStudioWindow::DecodeWorkerLoop, this);
        m_streamThread = std::thread(&EyeCamStudioWindow::PhoneStreamClientLoop, this);

        SetTimer(m_hwnd, 1, 16, nullptr);   // 60 FPS плавне апаратне оновлення Direct2D
        SetTimer(m_hwnd, 2, 4000, nullptr); // Авто-оновлення списку USB пристроїв кожні 4 с

        ShowWindow(m_hwnd, nCmdShow);
        UpdateWindow(m_hwnd);
        return true;
    }

    int RunMessageLoop() {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

private:
    void CreateTextFormats() {
        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI Variable Display", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            19.0f, L"uk-UA", &m_pFontTitle
        );
        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            13.0f, L"uk-UA", &m_pFontSection
        );
        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"uk-UA", &m_pFontBody
        );
        m_pDWriteFactory->CreateTextFormat(
            L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"uk-UA", &m_pFontMono
        );
    }

    void ReleaseOffscreenBlurTargets() {
        if (m_pSceneRT) { m_pSceneRT->Release(); m_pSceneRT = nullptr; }
        if (m_pBlurSmallRT) { m_pBlurSmallRT->Release(); m_pBlurSmallRT = nullptr; }
        if (m_pBlurPingRT) { m_pBlurPingRT->Release(); m_pBlurPingRT = nullptr; }
        m_blurTargetW = 0;
        m_blurTargetH = 0;
    }

    void Shutdown() {
        m_running = false;
        m_decodeCv.notify_all();
        m_sendCv.notify_all();
        CloseActivePhoneSocket();
        if (m_streamThread.joinable()) {
            m_streamThread.join();
        }
        if (m_decodeThread.joinable()) {
            m_decodeThread.join();
        }
        if (m_sendThread.joinable()) {
            m_sendThread.join();
        }
        {
            std::lock_guard<std::mutex> lock(m_sinkMutex);
            m_sink.Close();
        }

        ReleaseOffscreenBlurTargets();
        if (m_pPreviewBitmap) { m_pPreviewBitmap->Release(); m_pPreviewBitmap = nullptr; }
        if (m_pBrush) { m_pBrush->Release(); m_pBrush = nullptr; }
        if (m_pRenderTarget) { m_pRenderTarget->Release(); m_pRenderTarget = nullptr; }
        if (m_pFontTitle) { m_pFontTitle->Release(); m_pFontTitle = nullptr; }
        if (m_pFontSection) { m_pFontSection->Release(); m_pFontSection = nullptr; }
        if (m_pFontBody) { m_pFontBody->Release(); m_pFontBody = nullptr; }
        if (m_pFontMono) { m_pFontMono->Release(); m_pFontMono = nullptr; }
        if (m_pDWriteFactory) { m_pDWriteFactory->Release(); m_pDWriteFactory = nullptr; }
        if (m_pD2DFactory) { m_pD2DFactory->Release(); m_pD2DFactory = nullptr; }
        if (m_mfStarted) {
            MFShutdown();
            m_mfStarted = false;
        }
        CoUninitialize();
        WSACleanup();
    }

    static bool LoadSavedLanguageIsUa() {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\EyeCamStudio", 0, KEY_READ, &hKey) == ERROR_SUCCESS && hKey) {
            wchar_t val[16]{};
            DWORD sz = sizeof(val);
            DWORD type = 0;
            bool isUa = false;
            if (RegQueryValueExW(hKey, L"Language", nullptr, &type, reinterpret_cast<LPBYTE>(val), &sz) == ERROR_SUCCESS && type == REG_SZ) {
                isUa = (wcscmp(val, L"ua") == 0 || wcscmp(val, L"uk") == 0);
            }
            RegCloseKey(hKey);
            return isUa;
        }
        return false; // English (EN) default on first install
    }

    static void SaveLanguagePref(bool isUa) {
        HKEY hKey = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\EyeCamStudio", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS && hKey) {
            const wchar_t* val = isUa ? L"ua" : L"en";
            RegSetValueExW(hKey, L"Language", 0, REG_SZ, reinterpret_cast<const BYTE*>(val), static_cast<DWORD>((wcslen(val) + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }
    }

    const wchar_t* TR(const wchar_t* en, const wchar_t* ua) const {
        return m_state.isUkrainian ? ua : en;
    }

    static HICON LoadCustomStudioIcon(HINSTANCE hInstance) {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring dirPath(exePath);
        size_t pos = dirPath.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            dirPath = dirPath.substr(0, pos);
        }

        std::wstring cyberIconPath = dirPath + L"\\eyecam_cyber.ico";
        HICON hIco = static_cast<HICON>(LoadImageW(
            nullptr, cyberIconPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE
        ));
        if (hIco) return hIco;

        std::wstring primaryIconPath = dirPath + L"\\eyecam_pc.ico";
        hIco = static_cast<HICON>(LoadImageW(
            nullptr, primaryIconPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE
        ));
        if (hIco) return hIco;

        std::wstring fallbackIconPath = dirPath + L"\\..\\..\\src\\windows\\gui\\eyecam_cyber.ico";
        hIco = static_cast<HICON>(LoadImageW(
            nullptr, fallbackIconPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE
        ));
        if (hIco) return hIco;

        hIco = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
        if (hIco) return hIco;

        return LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }

    void CloseActivePhoneSocket() {
        std::lock_guard<std::mutex> lock(m_socketMutex);
        if (m_phoneSocket != INVALID_SOCKET) {
            closesocket(m_phoneSocket);
            m_phoneSocket = INVALID_SOCKET;
        }
    }

    void CheckGithubUpdateAsync() {
        if (m_state.updateCheckInProgress.exchange(true)) return;
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            m_state.updateStatusText = TR(L"Checking GitHub Releases for updates...", L"Перевірка оновлень на GitHub Releases...");
        }
        std::thread([this]() {
            std::string tag, htmlUrl;
            bool ok = QueryGithubLatestRelease(tag, htmlUrl);
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                if (ok) {
                    std::string cleanTag = tag;
                    if (!cleanTag.empty() && (cleanTag[0] == 'v' || cleanTag[0] == 'V')) {
                        cleanTag.erase(0, 1);
                    }
                    m_state.latestGithubTag = Utf8ToWide(cleanTag);
                    m_state.releaseHtmlUrl = Utf8ToWide(htmlUrl);
                    bool newer = IsVersionNewer(cleanTag, kEyeCamVersionStr);
                    m_state.updateAvailable = newer;
                    if (newer) {
                        m_state.updateStatusText = m_state.isUkrainian
                            ? (L"🚀 Доступне нове оновлення: v" + m_state.latestGithubTag + L"!")
                            : (L"🚀 New update available: v" + m_state.latestGithubTag + L"!");
                    } else {
                        m_state.updateStatusText = m_state.isUkrainian
                            ? (L"✓ У вас встановлена найновіша версія (v" + std::wstring(kEyeCamVersionWide) + L")")
                            : (L"✓ You are running the latest version (v" + std::wstring(kEyeCamVersionWide) + L")");
                    }
                } else {
                    m_state.updateStatusText = m_state.isUkrainian
                        ? L"Не вдалося перевірити реліз (перевірте інтернет або відкрийте GitHub)"
                        : L"Could not check releases (check internet connection or open GitHub)";
                }
            }
            m_state.updateCheckInProgress = false;
            if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
        }).detach();
    }

    void PromptAndLoadCubeLut() {
        wchar_t fileBuf[MAX_PATH]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hwnd;
        ofn.lpstrFilter = L"Adobe 3D LUT (*.cube)\0*.cube\0Усі файли (*.*)\0*.*\0";
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = L"Виберіть файл кольорового профілю .cube 3D LUT";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

        if (GetOpenFileNameW(&ofn)) {
            std::wstring loadedName;
            if (m_cubeLut.LoadFromFile(fileBuf, loadedName)) {
                m_state.activeLutName = loadedName;
            } else {
                MessageBoxW(
                    m_hwnd,
                    L"Не вдалося розпізнати таблицю LUT_3D_SIZE або LUT_1D_SIZE у вибраному .cube файлі.",
                    L"EyeCam Studio — Помилка .cube LUT",
                    MB_ICONWARNING | MB_OK
                );
            }
        }
    }

    void RefreshUsbDevicesAsync() {
        if (m_usbScanInProgress.exchange(true)) return;
        std::string preferredSerial;
        {
            std::lock_guard<std::mutex> lock(m_usbMutex);
            preferredSerial = m_state.selectedUsbSerial;
        }
        std::thread([this, preferredSerial]() {
            auto found = ScanConnectedUsbDevices(preferredSerial);
            {
                std::lock_guard<std::mutex> lock(m_usbMutex);
                m_state.usbDevices = std::move(found);
                int matchedIdx = -1;
                if (!m_state.selectedUsbSerial.empty()) {
                    for (size_t i = 0; i < m_state.usbDevices.size(); ++i) {
                        if (m_state.usbDevices[i].serial == m_state.selectedUsbSerial) {
                            matchedIdx = static_cast<int>(i);
                            break;
                        }
                    }
                }
                if (matchedIdx >= 0) {
                    m_state.selectedUsbIdx = matchedIdx;
                } else if (!m_state.usbDevices.empty()) {
                    m_state.selectedUsbIdx = 0;
                    m_state.selectedUsbSerial = m_state.usbDevices[0].serial;
                } else {
                    m_state.selectedUsbIdx = 0;
                }
            }
            m_usbScanInProgress = false;
            if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
        }).detach();
    }

    void EnqueueControlPacket(uint16_t msgType, std::string payload, const std::string& coalesceKey = "") {
        {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            if (!coalesceKey.empty()) {
                for (auto it = m_sendQueue.begin(); it != m_sendQueue.end();) {
                    if (it->coalesceKey == coalesceKey) {
                        it = m_sendQueue.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (m_sendQueue.size() >= 64) {
                m_sendQueue.pop_front();
            }
            m_sendQueue.push_back(OutboundControlPacket{msgType, std::move(payload), coalesceKey});
        }
        m_sendCv.notify_one();
    }

    void SendControlJson(const std::string& jsonPayload, const std::string& coalesceKey = "") {
        EnqueueControlPacket(1, jsonPayload, coalesceKey);
    }

    void SendKeyframeRequest() {
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
        long long prev = m_lastKeyframeReqMs.load();
        if (nowMs - prev < 350) return;
        m_lastKeyframeReqMs.store(nowMs);
        EnqueueControlPacket(5, "", "IDR_REQ");
    }

    void ControlSenderLoop() {
        std::vector<uint8_t> packetBuf;
        while (m_running) {
            OutboundControlPacket pkt;
            {
                std::unique_lock<std::mutex> lock(m_sendMutex);
                m_sendCv.wait(lock, [this]() {
                    return !m_running || !m_sendQueue.empty();
                });
                if (!m_running) break;
                pkt = std::move(m_sendQueue.front());
                m_sendQueue.pop_front();
            }

            uint32_t len = static_cast<uint32_t>(pkt.payload.size());
            packetBuf.resize(8 + len);
            packetBuf[0] = 0x43; // 'C'
            packetBuf[1] = 0x50; // 'P'
            packetBuf[2] = static_cast<uint8_t>((pkt.msgType >> 8) & 0xFF);
            packetBuf[3] = static_cast<uint8_t>(pkt.msgType & 0xFF);
            packetBuf[4] = static_cast<uint8_t>((len >> 24) & 0xFF);
            packetBuf[5] = static_cast<uint8_t>((len >> 16) & 0xFF);
            packetBuf[6] = static_cast<uint8_t>((len >> 8) & 0xFF);
            packetBuf[7] = static_cast<uint8_t>(len & 0xFF);
            if (len > 0) {
                std::memcpy(packetBuf.data() + 8, pkt.payload.data(), len);
            }

            std::lock_guard<std::mutex> sockLock(m_socketMutex);
            if (m_phoneSocket != INVALID_SOCKET) {
                size_t sentTotal = 0;
                while (sentTotal < packetBuf.size()) {
                    int n = send(
                        m_phoneSocket,
                        reinterpret_cast<const char*>(packetBuf.data() + sentTotal),
                        static_cast<int>(packetBuf.size() - sentTotal),
                        0
                    );
                    if (n <= 0) break;
                    sentTotal += static_cast<size_t>(n);
                }
            }
        }
    }

    static bool RecvExact(SOCKET s, uint8_t* buf, int len) {
        int total = 0;
        while (total < len) {
            int r = recv(s, reinterpret_cast<char*>(buf + total), len - total, 0);
            if (r <= 0) return false;
            total += r;
        }
        return true;
    }

    void EnqueueFlushDecoder() {
        {
            std::lock_guard<std::mutex> lock(m_decodeMutex);
            m_decodeQueue.clear();
            m_decodeQueue.push_back(IncomingVideoPacket{IncomingVideoPacket::Kind::FLUSH, true, {}});
        }
        m_decodeCv.notify_one();
    }

    void PhoneStreamClientLoop() {
        auto lastStatTime = std::chrono::steady_clock::now();
        uint64_t bytesWindow = 0;

        while (m_running) {
            if (!m_state.isStreaming) {
                m_state.isPhoneConnected = false;
                m_state.measuredBitrateKbps = 0;
                m_state.measuredFps = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                continue;
            }

            std::string targetIp = (m_state.transport == TransportTab::USB_ADB) ? "127.0.0.1" : m_state.deviceIp;

            SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET) {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                continue;
            }

            u_long nonBlocking = 1;
            ioctlsocket(sock, FIONBIO, &nonBlocking);

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(8554);
            if (inet_pton(AF_INET, targetIp.c_str(), &addr.sin_addr) <= 0) {
                closesocket(sock);
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                continue;
            }

            connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

            fd_set writeSet{};
            FD_ZERO(&writeSet);
            FD_SET(sock, &writeSet);
            timeval tv{0, 600000};
            if (select(0, nullptr, &writeSet, nullptr, &tv) <= 0) {
                closesocket(sock);
                m_state.isPhoneConnected = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            nonBlocking = 0;
            ioctlsocket(sock, FIONBIO, &nonBlocking);
            DWORD rcvTimeout = 2500;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcvTimeout), sizeof(rcvTimeout));
            int nodelay = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
            int rcvBufSize = 4 * 1024 * 1024;
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBufSize), sizeof(rcvBufSize));

            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                m_phoneSocket = sock;
            }
            {
                std::lock_guard<std::mutex> lock(m_sendMutex);
                m_sendQueue.clear();
            }
            m_state.isPhoneConnected = true;

            EnqueueFlushDecoder();
            bool waitForKeyframeEnqueue = true;

            SendControlJson("{\"action\":\"GetCapabilities\"}");

            std::vector<uint8_t> payload;
            while (m_running && m_state.isStreaming) {
                uint8_t hdr[8]{};
                if (!RecvExact(sock, hdr, 8)) break;

                uint16_t magic = static_cast<uint16_t>((hdr[0] << 8) | hdr[1]);
                uint16_t msgType = static_cast<uint16_t>((hdr[2] << 8) | hdr[3]);
                uint32_t len = (static_cast<uint32_t>(hdr[4]) << 24) |
                               (static_cast<uint32_t>(hdr[5]) << 16) |
                               (static_cast<uint32_t>(hdr[6]) << 8) |
                               static_cast<uint32_t>(hdr[7]);

                if (magic != 0x4350 || len > 8 * 1024 * 1024) break;

                payload.resize(len);
                if (len > 0 && !RecvExact(sock, payload.data(), static_cast<int>(len))) break;

                bytesWindow += (8 + len);
                m_state.rxBytes += (8 + len);

                if (msgType == 8 && !payload.empty()) {
                    bool isKey = IsH264KeyFrameAnnexB(payload.data(), payload.size());
                    bool needIdr = false;
                    {
                        std::lock_guard<std::mutex> lock(m_decodeMutex);
                        if (isKey) {
                            bool hadFlush = (!m_decodeQueue.empty() &&
                                             m_decodeQueue.front().kind == IncomingVideoPacket::Kind::FLUSH);
                            m_decodeQueue.clear();
                            if (hadFlush) {
                                m_decodeQueue.push_back(IncomingVideoPacket{IncomingVideoPacket::Kind::FLUSH, true, {}});
                            }
                            waitForKeyframeEnqueue = false;
                            m_decodeQueue.push_back(IncomingVideoPacket{IncomingVideoPacket::Kind::H264, true, std::move(payload)});
                            m_decodeCv.notify_one();
                        } else if (waitForKeyframeEnqueue) {
                            needIdr = true;
                        } else if (m_decodeQueue.size() >= 6) {
                            bool hadFlush = (!m_decodeQueue.empty() &&
                                             m_decodeQueue.front().kind == IncomingVideoPacket::Kind::FLUSH);
                            m_decodeQueue.clear();
                            if (hadFlush) {
                                m_decodeQueue.push_back(IncomingVideoPacket{IncomingVideoPacket::Kind::FLUSH, true, {}});
                            }
                            waitForKeyframeEnqueue = true;
                            needIdr = true;
                        } else {
                            m_decodeQueue.push_back(IncomingVideoPacket{IncomingVideoPacket::Kind::H264, false, std::move(payload)});
                            m_decodeCv.notify_one();
                        }
                    }
                    if (needIdr) {
                        SendKeyframeRequest();
                    }
                } else if (msgType == 7 && !payload.empty()) {
                    std::lock_guard<std::mutex> lock(m_decodeMutex);
                    for (auto it = m_decodeQueue.begin(); it != m_decodeQueue.end();) {
                        if (it->kind == IncomingVideoPacket::Kind::JPEG) {
                            it = m_decodeQueue.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    m_decodeQueue.push_back(IncomingVideoPacket{IncomingVideoPacket::Kind::JPEG, true, std::move(payload)});
                    m_decodeCv.notify_one();
                } else if (msgType == 1 && !payload.empty()) {
                    std::string jsonStr(reinterpret_cast<const char*>(payload.data()), payload.size());
                    ParseDeviceCapabilitiesJson(jsonStr);
                }

                auto now = std::chrono::steady_clock::now();
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatTime).count();
                if (elapsedMs >= 1000) {
                    m_state.measuredBitrateKbps = static_cast<uint32_t>((bytesWindow * 8) / static_cast<uint64_t>(elapsedMs));
                    bytesWindow = 0;
                    lastStatTime = now;
                }
            }

            CloseActivePhoneSocket();
            EnqueueFlushDecoder();
            m_state.isPhoneConnected = false;
            m_state.measuredBitrateKbps = 0;
            m_state.measuredFps = 0;
        }
    }

    void DecodeWorkerLoop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* pWicFactory = nullptr;
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&pWicFactory)
        );

        MfH264Decoder h264Decoder;
        bool needIdrAfterFlush = true;
        uint32_t decodedWindow = 0;
        auto lastFpsTime = std::chrono::steady_clock::now();

        while (m_running) {
            IncomingVideoPacket pkt;
            {
                std::unique_lock<std::mutex> lock(m_decodeMutex);
                m_decodeCv.wait(lock, [this]() {
                    return !m_running || !m_decodeQueue.empty();
                });
                if (!m_running) break;
                pkt = std::move(m_decodeQueue.front());
                m_decodeQueue.pop_front();
            }

            if (pkt.kind == IncomingVideoPacket::Kind::FLUSH) {
                h264Decoder.Flush();
                needIdrAfterFlush = true;
                decodedWindow = 0;
                lastFpsTime = std::chrono::steady_clock::now();
                continue;
            }

            if (pkt.kind == IncomingVideoPacket::Kind::H264) {
                if (needIdrAfterFlush && !pkt.isKeyFrame) {
                    SendKeyframeRequest();
                    continue;
                }
                if (pkt.isKeyFrame) {
                    needIdrAfterFlush = false;
                }
                h264Decoder.DecodeAccessUnit(
                    pkt.data.data(),
                    pkt.data.size(),
                    [this, &decodedWindow](const uint8_t* pNv12, LONG stride, uint32_t codedW, uint32_t codedH,
                                           uint32_t visX, uint32_t visY, uint32_t visW, uint32_t visH) {
                        PushNv12ToDirectShowAndPreview(pNv12, stride, codedW, codedH, visX, visY, visW, visH);
                        m_state.rxFrames++;
                        decodedWindow++;
                    }
                );
            } else if (pkt.kind == IncomingVideoPacket::Kind::JPEG) {
                DecodeIncomingJpegFrame(pWicFactory, pkt.data.data(), pkt.data.size());
                m_state.rxFrames++;
                decodedWindow++;
            }

            auto now = std::chrono::steady_clock::now();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFpsTime).count();
            if (elapsedMs >= 1000) {
                m_state.measuredFps = static_cast<uint32_t>(
                    (static_cast<uint64_t>(decodedWindow) * 1000) / static_cast<uint64_t>(elapsedMs)
                );
                decodedWindow = 0;
                lastFpsTime = now;
            }
        }

        h264Decoder.Shutdown();
        if (pWicFactory) {
            pWicFactory->Release();
        }
        CoUninitialize();
    }

    static int ModuleKeyToIndex(const std::string& key) {
        if (key == "uw") return 0;
        if (key == "main") return 1;
        if (key == "tele") return 2;
        if (key == "tele5x") return 3;
        if (key == "front") return 4;
        return 1;
    }

    void ParseDeviceCapabilitiesJson(const std::string& json) {
        std::string devName;
        if (ExtractJsonString(json, "device_name", devName) && !devName.empty()) {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            m_state.connectedPhoneName = Utf8ToWide(devName);
        }

        bool sup4k = true;
        if (ExtractJsonBool(json, "supports_4k", sup4k)) {
            m_state.phoneSupports4K = sup4k;
        }
        bool sup2k = true;
        if (ExtractJsonBool(json, "supports_2k", sup2k)) {
            m_state.phoneSupports2K = sup2k;
        }
        bool sup60 = true;
        if (ExtractJsonBool(json, "supports_60fps", sup60)) {
            m_state.phoneSupports60Fps = sup60;
        }
        bool lowEnd = false;
        if (ExtractJsonBool(json, "is_low_end", lowEnd)) {
            m_state.phoneIsLowEnd = lowEnd;
        }

        // Отримання наявності фізичних модулів камери смартфона
        bool hasUw = false, hasMain = true, hasTele = false, hasTele5x = false, hasFront = true;
        if (ExtractJsonBool(json, "has_uw", hasUw)) m_state.moduleAvailable[0] = hasUw;
        if (ExtractJsonBool(json, "has_main", hasMain)) m_state.moduleAvailable[1] = hasMain;
        if (ExtractJsonBool(json, "has_tele", hasTele)) m_state.moduleAvailable[2] = hasTele;
        if (ExtractJsonBool(json, "has_tele5x", hasTele5x)) m_state.moduleAvailable[3] = hasTele5x;
        if (ExtractJsonBool(json, "has_front", hasFront)) m_state.moduleAvailable[4] = hasFront;

        std::string activeModKey;
        if (ExtractJsonString(json, "active_module", activeModKey) && !activeModKey.empty()) {
            m_state.activeModuleIdx = ModuleKeyToIndex(activeModKey);
        }

        float maxIntraZ = 0.0f;
        if (ExtractJsonFloat(json, "max_intra_zoom", maxIntraZ) && maxIntraZ >= 1.0f) {
            m_state.maxIntraModuleZoom = maxIntraZ;
        }
        float intraZ = 0.0f;
        bool hasIntraZ = ExtractJsonFloat(json, "intra_zoom", intraZ);
        if (!hasIntraZ) {
            hasIntraZ = ExtractJsonFloat(json, "current_zoom", intraZ);
        }
        if (m_activeSliderDrag.load() != 100 && hasIntraZ && intraZ >= 1.0f) {
            m_state.intraModuleZoom = std::clamp(intraZ, 1.0f, m_state.maxIntraModuleZoom);
            float span = (std::max)(0.1f, m_state.maxIntraModuleZoom - 1.0f);
            m_state.zoomSliderNorm = std::clamp((m_state.intraModuleZoom - 1.0f) / span, 0.0f, 1.0f);
        }

        bool torchOn = false;
        if (ExtractJsonBool(json, "torch_on", torchOn)) {
            m_state.torchOn = torchOn;
        }
        bool torchAvail = true;
        if (ExtractJsonBool(json, "torch_available", torchAvail)) {
            m_state.torchAvailable = torchAvail;
        }
        bool aeLock = false;
        if (ExtractJsonBool(json, "ae_locked", aeLock)) {
            m_state.aeLocked = aeLock;
        }

        // Синхронізація замочків повзунків (якщо ми зараз не тягнемо повзунок)
        if (m_activeSliderDrag.load() < 0 || m_activeSliderDrag.load() >= 4) {
            bool bVal = false;
            if (ExtractJsonBool(json, "iso_manual", bVal)) m_state.sliderManual[0] = bVal;
            if (ExtractJsonBool(json, "shutter_manual", bVal)) m_state.sliderManual[1] = bVal;
            if (ExtractJsonBool(json, "focus_manual", bVal)) m_state.sliderManual[2] = bVal;
            if (ExtractJsonBool(json, "wb_manual", bVal)) m_state.sliderManual[3] = bVal;
        }

        int rotDeg = -1;
        if (ExtractJsonInt(json, "rotation_deg", rotDeg)) {
            if (rotDeg == -1 || rotDeg == 0 || rotDeg == 90 || rotDeg == 180 || rotDeg == 270) {
                m_state.streamRotationDeg = rotDeg;
            }
        }

        int sw = 0, sh = 0, sfps = 0;
        if (ExtractJsonInt(json, "stream_width", sw) && ExtractJsonInt(json, "stream_height", sh) && sw > 0 && sh > 0) {
            ExtractJsonInt(json, "stream_fps", sfps);
            uint32_t newW = static_cast<uint32_t>(sw);
            uint32_t newH = static_cast<uint32_t>(sh);
            uint32_t newFps = (sfps > 0) ? static_cast<uint32_t>(sfps) : m_state.fps;
            std::lock_guard<std::mutex> lock(m_sinkMutex);
            if (m_state.width != newW || m_state.height != newH || m_state.fps != newFps) {
                m_state.width = newW;
                m_state.height = newH;
                m_state.fps = newFps;
                if (newW >= 3840) m_state.resPresetIdx = 0;
                else if (newW >= 2560) m_state.resPresetIdx = 1;
                else if (newW >= 1920) m_state.resPresetIdx = 2;
                else m_state.resPresetIdx = 3;
                if (m_sink.IsOpen()) {
                    m_sink.SetFormat(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2, m_state.fps);
                }
            }
        }

        float therm = 0.0f;
        if (ExtractJsonFloat(json, "thermal_temp_c", therm) && therm > 0.0f) {
            m_state.phoneThermalC = therm;
        }
        float batt = 0.0f;
        if (ExtractJsonFloat(json, "battery_level", batt) && batt > 0.0f) {
            m_state.phoneBatteryPct = static_cast<int>(batt + 0.5f);
        }
        int phoneFps = 0;
        if (ExtractJsonInt(json, "measured_fps", phoneFps) && phoneFps >= 0) {
            m_state.phoneMeasuredFps = phoneFps;
        }
        int phoneKbps = 0;
        if (ExtractJsonInt(json, "measured_bitrate_kbps", phoneKbps) && phoneKbps >= 0) {
            m_state.phoneMeasuredBitrateKbps = phoneKbps;
        }
    }

    // Високошвидкісна конвертація NV12 -> DirectShow YUY2 та 640x360 BGRA прев'ю (з підтримкою .cube 3D LUT)
    void PushNv12ToDirectShowAndPreview(
        const uint8_t* pScan0,
        LONG stride,
        uint32_t codedW,
        uint32_t codedH,
        uint32_t visX,
        uint32_t visY,
        uint32_t visW,
        uint32_t visH
    ) {
        (void)codedW;
        if (!pScan0 || visW < 2 || visH < 2) return;

        uint32_t outW = 1920;
        uint32_t outH = 1080;
        {
            std::lock_guard<std::mutex> lock(m_sinkMutex);
            outW = m_state.width;
            outH = m_state.height;
        }
        if (outW < 2 || outH < 2) return;
        const bool mirror = m_state.mirrorHorizontal;
        const bool useLut = m_cubeLut.IsLoaded();

        const uint8_t* pUVBase = pScan0 + static_cast<ptrdiff_t>(codedH) * stride;
        size_t expectedYuy2 = EyeCam::CalculateFrameSize(outW, outH, EyeCam::PixelFormat::YUY2);
        if (m_yuy2Buffer.size() != expectedYuy2) {
            m_yuy2Buffer.resize(expectedYuy2);
        }

        if (!useLut) {
            if (visW == outW && visH == outH && !mirror && (visX & 1u) == 0 && (visY & 1u) == 0) {
                for (uint32_t y = 0; y < outH; ++y) {
                    const uint8_t* yRow = pScan0 + static_cast<ptrdiff_t>(visY + y) * stride + visX;
                    const uint8_t* uvRow = pUVBase + static_cast<ptrdiff_t>((visY >> 1) + (y >> 1)) * stride + visX;
                    uint8_t* dstRow = m_yuy2Buffer.data() + static_cast<size_t>(y) * outW * 2;
                    for (uint32_t x = 0; x < outW; x += 2) {
                        size_t dIdx = static_cast<size_t>(x) * 2;
                        dstRow[dIdx + 0] = yRow[x];
                        dstRow[dIdx + 1] = uvRow[x];
                        dstRow[dIdx + 2] = yRow[x + 1];
                        dstRow[dIdx + 3] = uvRow[x + 1];
                    }
                }
            } else {
                for (uint32_t y = 0; y < outH; ++y) {
                    uint32_t sy = visY + static_cast<uint32_t>((static_cast<uint64_t>(y) * visH) / outH);
                    const uint8_t* yRow = pScan0 + static_cast<ptrdiff_t>(sy) * stride;
                    const uint8_t* uvRow = pUVBase + static_cast<ptrdiff_t>(sy >> 1) * stride;
                    uint8_t* dstRow = m_yuy2Buffer.data() + static_cast<size_t>(y) * outW * 2;
                    for (uint32_t x = 0; x < outW; x += 2) {
                        uint32_t dx0 = mirror ? (outW - 1 - x) : x;
                        uint32_t dx1 = mirror ? (outW - 2 - x) : (x + 1);
                        uint32_t sx0 = visX + static_cast<uint32_t>((static_cast<uint64_t>(dx0) * visW) / outW);
                        uint32_t sx1 = visX + static_cast<uint32_t>((static_cast<uint64_t>(dx1) * visW) / outW);
                        uint32_t uvPair = (sx0 & ~1u);
                        size_t dIdx = static_cast<size_t>(x) * 2;
                        dstRow[dIdx + 0] = yRow[sx0];
                        dstRow[dIdx + 1] = uvRow[uvPair];
                        dstRow[dIdx + 2] = yRow[sx1];
                        dstRow[dIdx + 3] = uvRow[uvPair + 1];
                    }
                }
            }
        } else {
            // Застосування 3D LUT .cube до вихідного потоку DirectShow Virtual Camera
            for (uint32_t y = 0; y < outH; ++y) {
                uint32_t sy = visY + static_cast<uint32_t>((static_cast<uint64_t>(y) * visH) / outH);
                const uint8_t* yRow = pScan0 + static_cast<ptrdiff_t>(sy) * stride;
                const uint8_t* uvRow = pUVBase + static_cast<ptrdiff_t>(sy >> 1) * stride;
                uint8_t* dstRow = m_yuy2Buffer.data() + static_cast<size_t>(y) * outW * 2;
                for (uint32_t x = 0; x < outW; x += 2) {
                    uint32_t dx0 = mirror ? (outW - 1 - x) : x;
                    uint32_t dx1 = mirror ? (outW - 2 - x) : (x + 1);
                    uint32_t sx0 = visX + static_cast<uint32_t>((static_cast<uint64_t>(dx0) * visW) / outW);
                    uint32_t sx1 = visX + static_cast<uint32_t>((static_cast<uint64_t>(dx1) * visW) / outW);
                    uint32_t uvPair = (sx0 & ~1u);

                    int c0 = static_cast<int>(yRow[sx0]) - 16;
                    int c1 = static_cast<int>(yRow[sx1]) - 16;
                    int d = static_cast<int>(uvRow[uvPair]) - 128;
                    int e = static_cast<int>(uvRow[uvPair + 1]) - 128;

                    int r0 = (298 * c0 + 409 * e + 128) >> 8;
                    int g0 = (298 * c0 - 100 * d - 208 * e + 128) >> 8;
                    int b0 = (298 * c0 + 516 * d + 128) >> 8;

                    int r1 = (298 * c1 + 409 * e + 128) >> 8;
                    int g1 = (298 * c1 - 100 * d - 208 * e + 128) >> 8;
                    int b1 = (298 * c1 + 516 * d + 128) >> 8;

                    m_cubeLut.ApplyRgb(r0, g0, b0);
                    m_cubeLut.ApplyRgb(r1, g1, b1);

                    uint8_t yOut0 = static_cast<uint8_t>(((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16);
                    uint8_t yOut1 = static_cast<uint8_t>(((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16);
                    int rAvg = (r0 + r1) >> 1;
                    int gAvg = (g0 + g1) >> 1;
                    int bAvg = (b0 + b1) >> 1;
                    uint8_t uOut = static_cast<uint8_t>(((-38 * rAvg - 74 * gAvg + 112 * bAvg + 128) >> 8) + 128);
                    uint8_t vOut = static_cast<uint8_t>(((112 * rAvg - 94 * gAvg - 18 * bAvg + 128) >> 8) + 128);

                    size_t dIdx = static_cast<size_t>(x) * 2;
                    dstRow[dIdx + 0] = yOut0;
                    dstRow[dIdx + 1] = uOut;
                    dstRow[dIdx + 2] = yOut1;
                    dstRow[dIdx + 3] = vOut;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_sinkMutex);
            if (m_sink.IsOpen() && outW == m_state.width && outH == m_state.height) {
                if (m_sink.WriteFrame(m_yuy2Buffer.data(), m_yuy2Buffer.size())) {
                    m_state.shmFramesWritten++;
                }
            }
        }

        const uint32_t pw = 640;
        const uint32_t ph = 360;
        const size_t previewSize = static_cast<size_t>(pw) * ph * 4;
        if (m_cachedPreviewBuffer.size() != previewSize) {
            m_cachedPreviewBuffer.resize(previewSize);
        }

        auto clamp8 = [](int v) -> uint8_t {
            return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        };

        for (uint32_t y = 0; y < ph; ++y) {
            uint32_t sy = visY + static_cast<uint32_t>((static_cast<uint64_t>(y) * visH) / ph);
            const uint8_t* yRow = pScan0 + static_cast<ptrdiff_t>(sy) * stride;
            const uint8_t* uvRow = pUVBase + static_cast<ptrdiff_t>(sy >> 1) * stride;
            uint8_t* dstRow = m_cachedPreviewBuffer.data() + static_cast<size_t>(y) * pw * 4;

            for (uint32_t x = 0; x < pw; ++x) {
                uint32_t dx = mirror ? (pw - 1 - x) : x;
                uint32_t sx = visX + static_cast<uint32_t>((static_cast<uint64_t>(dx) * visW) / pw);
                uint32_t uvPair = (sx & ~1u);

                int cVal = static_cast<int>(yRow[sx]) - 16;
                int dVal = static_cast<int>(uvRow[uvPair]) - 128;
                int eVal = static_cast<int>(uvRow[uvPair + 1]) - 128;

                int r = (298 * cVal + 409 * eVal + 128) >> 8;
                int g = (298 * cVal - 100 * dVal - 208 * eVal + 128) >> 8;
                int b = (298 * cVal + 516 * dVal + 128) >> 8;

                if (useLut) {
                    m_cubeLut.ApplyRgb(r, g, b);
                }

                size_t dIdx = static_cast<size_t>(x) * 4;
                dstRow[dIdx + 0] = clamp8(b);
                dstRow[dIdx + 1] = clamp8(g);
                dstRow[dIdx + 2] = clamp8(r);
                dstRow[dIdx + 3] = 255;
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            m_previewPixels = m_cachedPreviewBuffer;
            m_hasLiveFrame = true;
        }
        if (m_hwnd) {
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }

    void DecodeIncomingJpegFrame(IWICImagingFactory* pWicFactory, const uint8_t* jpegData, size_t jpegLen) {
        if (!pWicFactory || !jpegData || jpegLen == 0) return;

        IWICStream* pStream = nullptr;
        HRESULT hr = pWicFactory->CreateStream(&pStream);
        if (FAILED(hr) || !pStream) return;

        hr = pStream->InitializeFromMemory(
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(jpegData)),
            static_cast<DWORD>(jpegLen)
        );
        if (FAILED(hr)) {
            pStream->Release();
            return;
        }

        IWICBitmapDecoder* pDecoder = nullptr;
        hr = pWicFactory->CreateDecoderFromStream(
            pStream, nullptr, WICDecodeMetadataCacheOnDemand, &pDecoder
        );
        if (SUCCEEDED(hr) && pDecoder) {
            IWICBitmapFrameDecode* pFrame = nullptr;
            hr = pDecoder->GetFrame(0, &pFrame);
            if (SUCCEEDED(hr) && pFrame) {
                UINT imgW = 0, imgH = 0;
                pFrame->GetSize(&imgW, &imgH);

                if (imgW > 0 && imgH > 0) {
                    IWICFormatConverter* pFullConverter = nullptr;
                    hr = pWicFactory->CreateFormatConverter(&pFullConverter);
                    if (SUCCEEDED(hr) && pFullConverter) {
                        hr = pFullConverter->Initialize(
                            pFrame,
                            GUID_WICPixelFormat32bppBGRA,
                            WICBitmapDitherTypeNone,
                            nullptr,
                            0.0,
                            WICBitmapPaletteTypeCustom
                        );
                        if (SUCCEEDED(hr)) {
                            size_t requiredBgra = static_cast<size_t>(imgW) * imgH * 4;
                            if (m_fullBgraBuffer.size() != requiredBgra) {
                                m_fullBgraBuffer.resize(requiredBgra);
                            }
                            hr = pFullConverter->CopyPixels(
                                nullptr,
                                imgW * 4,
                                static_cast<UINT>(requiredBgra),
                                m_fullBgraBuffer.data()
                            );
                            if (SUCCEEDED(hr)) {
                                if (m_state.mirrorHorizontal) {
                                    for (UINT y = 0; y < imgH; ++y) {
                                        uint8_t* row = m_fullBgraBuffer.data() + y * imgW * 4;
                                        for (UINT x = 0; x < imgW / 2; ++x) {
                                            UINT rx = imgW - 1 - x;
                                            for (int c = 0; c < 4; ++c) {
                                                std::swap(row[x * 4 + c], row[rx * 4 + c]);
                                            }
                                        }
                                    }
                                }
                                if (m_cubeLut.IsLoaded()) {
                                    size_t numPix = static_cast<size_t>(imgW) * imgH;
                                    uint8_t* p = m_fullBgraBuffer.data();
                                    for (size_t i = 0; i < numPix; ++i) {
                                        int b = p[0], g = p[1], r = p[2];
                                        m_cubeLut.ApplyRgb(r, g, b);
                                        p[0] = static_cast<uint8_t>(b);
                                        p[1] = static_cast<uint8_t>(g);
                                        p[2] = static_cast<uint8_t>(r);
                                        p += 4;
                                    }
                                }
                                PushFrameToDirectShow(m_fullBgraBuffer.data(), imgW, imgH);

                                const uint32_t pw = 640;
                                const uint32_t ph = 360;
                                size_t previewSize = static_cast<size_t>(pw) * ph * 4;
                                if (m_cachedPreviewBuffer.size() != previewSize) {
                                    m_cachedPreviewBuffer.resize(previewSize);
                                }

                                const uint32_t* srcPixels = reinterpret_cast<const uint32_t*>(m_fullBgraBuffer.data());
                                uint32_t* dstPixels = reinterpret_cast<uint32_t*>(m_cachedPreviewBuffer.data());

                                for (uint32_t y = 0; y < ph; ++y) {
                                    uint32_t srcY = (y * imgH) / ph;
                                    const uint32_t* srcRow = srcPixels + srcY * imgW;
                                    uint32_t* dstRow = dstPixels + y * pw;
                                    for (uint32_t x = 0; x < pw; ++x) {
                                        uint32_t srcX = (x * imgW) / pw;
                                        dstRow[x] = srcRow[srcX];
                                    }
                                }

                                {
                                    std::lock_guard<std::mutex> lock(m_frameMutex);
                                    m_previewPixels = m_cachedPreviewBuffer;
                                    m_hasLiveFrame = true;
                                }
                                if (m_hwnd) {
                                    InvalidateRect(m_hwnd, nullptr, FALSE);
                                }
                            }
                        }
                        pFullConverter->Release();
                    }
                }
                pFrame->Release();
            }
            pDecoder->Release();
        }
        pStream->Release();
    }

    void PushFrameToDirectShow(const uint8_t* srcBgra, uint32_t inW, uint32_t inH) {
        if (!srcBgra || inW == 0 || inH == 0) return;

        uint32_t outW = 1920;
        uint32_t outH = 1080;
        {
            std::lock_guard<std::mutex> lock(m_sinkMutex);
            outW = m_state.width;
            outH = m_state.height;
        }
        size_t expectedYuy2 = EyeCam::CalculateFrameSize(outW, outH, EyeCam::PixelFormat::YUY2);
        if (m_yuy2Buffer.size() != expectedYuy2) {
            m_yuy2Buffer.resize(expectedYuy2);
        }

        if (inW == outW && inH == outH) {
            const size_t totalPairs = (static_cast<size_t>(outW) * outH) / 2;
            const uint8_t* pSrc = srcBgra;
            uint8_t* pDst = m_yuy2Buffer.data();

            for (size_t i = 0; i < totalPairs; ++i) {
                int b0 = pSrc[0], g0 = pSrc[1], r0 = pSrc[2];
                int b1 = pSrc[4], g1 = pSrc[5], r1 = pSrc[6];
                pSrc += 8;

                uint8_t y0 = static_cast<uint8_t>(((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16);
                uint8_t y1 = static_cast<uint8_t>(((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16);

                int rAvg = (r0 + r1) >> 1;
                int gAvg = (g0 + g1) >> 1;
                int bAvg = (b0 + b1) >> 1;

                uint8_t u = static_cast<uint8_t>(((-38 * rAvg - 74 * gAvg + 112 * bAvg + 128) >> 8) + 128);
                uint8_t v = static_cast<uint8_t>(((112 * rAvg - 94 * gAvg - 18 * bAvg + 128) >> 8) + 128);

                pDst[0] = y0;
                pDst[1] = u;
                pDst[2] = y1;
                pDst[3] = v;
                pDst += 4;
            }
        } else {
            for (uint32_t y = 0; y < outH; ++y) {
                uint32_t srcY = (static_cast<uint64_t>(y) * inH) / outH;
                const uint8_t* srcRow = srcBgra + (srcY * inW * 4);
                uint8_t* dstRow = m_yuy2Buffer.data() + (y * outW * 2);

                for (uint32_t x = 0; x < outW; x += 2) {
                    uint32_t srcX0 = (static_cast<uint64_t>(x) * inW) / outW;
                    uint32_t srcX1 = (static_cast<uint64_t>(x + 1) * inW) / outW;

                    const uint8_t* p0 = srcRow + (srcX0 * 4);
                    const uint8_t* p1 = srcRow + (srcX1 * 4);

                    int b0 = p0[0], g0 = p0[1], r0 = p0[2];
                    int b1 = p1[0], g1 = p1[1], r1 = p1[2];

                    uint8_t y0 = static_cast<uint8_t>(((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16);
                    uint8_t y1 = static_cast<uint8_t>(((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16);

                    int rAvg = (r0 + r1) >> 1;
                    int gAvg = (g0 + g1) >> 1;
                    int bAvg = (b0 + b1) >> 1;

                    uint8_t u = static_cast<uint8_t>(((-38 * rAvg - 74 * gAvg + 112 * bAvg + 128) >> 8) + 128);
                    uint8_t v = static_cast<uint8_t>(((112 * rAvg - 94 * gAvg - 18 * bAvg + 128) >> 8) + 128);

                    size_t dIdx = x * 2;
                    dstRow[dIdx + 0] = y0;
                    dstRow[dIdx + 1] = u;
                    dstRow[dIdx + 2] = y1;
                    dstRow[dIdx + 3] = v;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_sinkMutex);
            if (m_sink.IsOpen() && outW == m_state.width && outH == m_state.height) {
                if (m_sink.WriteFrame(m_yuy2Buffer.data(), m_yuy2Buffer.size())) {
                    m_state.shmFramesWritten++;
                }
            }
        }
    }

    void EnsureRenderTarget() {
        if (!m_pRenderTarget) {
            RECT rc{};
            GetClientRect(m_hwnd, &rc);
            UINT32 w = (std::max)(1u, static_cast<UINT32>(rc.right - rc.left));
            UINT32 h = (std::max)(1u, static_cast<UINT32>(rc.bottom - rc.top));
            D2D1_SIZE_U size = D2D1::SizeU(w, h);

            m_pD2DFactory->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(m_hwnd, size),
                &m_pRenderTarget
            );

            if (m_pRenderTarget) {
                m_pRenderTarget->CreateSolidColorBrush(COL_TEXT_PRIMARY, &m_pBrush);

                D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)
                );
                m_pRenderTarget->CreateBitmap(D2D1::SizeU(640, 360), nullptr, 0, bmpProps, &m_pPreviewBitmap);
            }
        }
    }

    bool EnsureBlurTargets(float W, float H) {
        if (!m_pRenderTarget || W < 16.0f || H < 16.0f) return false;
        UINT32 uw = static_cast<UINT32>(W);
        UINT32 uh = static_cast<UINT32>(H);
        if (m_pSceneRT && m_pBlurSmallRT && m_pBlurPingRT && m_blurTargetW == uw && m_blurTargetH == uh) {
            return true;
        }
        ReleaseOffscreenBlurTargets();

        D2D1_SIZE_F fullSize = D2D1::SizeF(W, H);
        D2D1_SIZE_F smallSize = D2D1::SizeF((std::max)(32.0f, W / 6.0f), (std::max)(32.0f, H / 6.0f));
        if (FAILED(m_pRenderTarget->CreateCompatibleRenderTarget(fullSize, &m_pSceneRT)) || !m_pSceneRT) {
            ReleaseOffscreenBlurTargets();
            return false;
        }
        if (FAILED(m_pRenderTarget->CreateCompatibleRenderTarget(smallSize, &m_pBlurSmallRT)) || !m_pBlurSmallRT) {
            ReleaseOffscreenBlurTargets();
            return false;
        }
        if (FAILED(m_pRenderTarget->CreateCompatibleRenderTarget(smallSize, &m_pBlurPingRT)) || !m_pBlurPingRT) {
            ReleaseOffscreenBlurTargets();
            return false;
        }
        m_blurTargetW = uw;
        m_blurTargetH = uh;
        return true;
    }

    void DrawRoundedCard(const UiRect& r, const D2D1_COLOR_F& fill, const D2D1_COLOR_F& border, float radius = 10.0f) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r.ToD2D(), radius, radius);
        m_pBrush->SetColor(fill);
        m_activeRT->FillRoundedRectangle(rr, m_pBrush);
        m_pBrush->SetColor(border);
        m_activeRT->DrawRoundedRectangle(rr, m_pBrush, 1.0f);
    }

    void DrawTextStr(const std::wstring& text, const UiRect& r, IDWriteTextFormat* fmt, const D2D1_COLOR_F& color,
                     DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!fmt || !m_activeRT) return;
        fmt->SetTextAlignment(align);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_pBrush->SetColor(color);
        m_activeRT->DrawText(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            fmt,
            r.ToD2D(),
            m_pBrush
        );
    }

    // Уніфікована фірмова іконка Cyber Eye Camera Lens (єдиний стиль із мобільним додатком та GitHub README)
    void DrawCyberEyeLogo(float x, float y, float size) {
        if (!m_activeRT) return;
        float cx = x + size * 0.5f;
        float cy = y + size * 0.5f;
        float rOuter = size * 0.44f;

        // 1. Заокруглений темний корпус-сквіркл (#0B0E14) із смарагдово-ціановою рамкою
        UiRect sqRect{x, y, x + size, y + size};
        DrawRoundedCard(sqRect, HexColor(0x0B0E14), COL_EMERALD, size * 0.24f);

        // 2. Зовнішнє технічне кільце об'єктива (#1E2638) + 4 мітки фокуса
        m_pBrush->SetColor(HexColor(0x1E2638));
        m_activeRT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rOuter, rOuter), m_pBrush, 1.4f);

        m_pBrush->SetColor(COL_EMERALD);
        m_activeRT->DrawLine(D2D1::Point2F(cx, cy - rOuter - 1.0f), D2D1::Point2F(cx, cy - rOuter + 2.5f), m_pBrush, 1.5f);
        m_activeRT->DrawLine(D2D1::Point2F(cx, cy + rOuter - 2.5f), D2D1::Point2F(cx, cy + rOuter + 1.0f), m_pBrush, 1.5f);
        m_activeRT->DrawLine(D2D1::Point2F(cx - rOuter - 1.0f, cy), D2D1::Point2F(cx - rOuter + 2.5f, cy), m_pBrush, 1.5f);
        m_activeRT->DrawLine(D2D1::Point2F(cx + rOuter - 2.5f, cy), D2D1::Point2F(cx + rOuter + 1.0f, cy), m_pBrush, 1.5f);

        // 3. Смарагдовий контур кібер-ока (#00E676)
        m_pBrush->SetColor(HexColor(0x0A1622));
        m_activeRT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), size * 0.37f, size * 0.23f), m_pBrush);
        m_pBrush->SetColor(COL_EMERALD);
        m_activeRT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), size * 0.37f, size * 0.23f), m_pBrush, 2.0f);

        // 4. Райдужка діафрагми (#00E5FF)
        float rIris = size * 0.19f;
        m_pBrush->SetColor(HexColor(0x0D1B2A));
        m_activeRT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rIris, rIris), m_pBrush);
        m_pBrush->SetColor(COL_CYAN);
        m_activeRT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rIris, rIris), m_pBrush, 1.7f);

        // Внутрішнє кільце діафрагми
        float rAperture = size * 0.14f;
        m_pBrush->SetColor(HexColor(0x00E676, 0.55f));
        m_activeRT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rAperture, rAperture), m_pBrush, 1.0f);

        // 5. Глибока зіниця об'єктива (#030508)
        float rPupil = size * 0.085f;
        m_pBrush->SetColor(HexColor(0x030508));
        m_activeRT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rPupil, rPupil), m_pBrush);

        // 6. Головний білий блік лінзи + вторинний ціановий рефлекс
        m_pBrush->SetColor(HexColor(0xFFFFFF, 0.94f));
        m_activeRT->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx - size * 0.065f, cy - size * 0.065f), size * 0.042f, size * 0.032f),
            m_pBrush
        );
        m_pBrush->SetColor(HexColor(0x00E5FF, 0.75f));
        m_activeRT->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx + size * 0.06f, cy + size * 0.06f), size * 0.022f, size * 0.022f),
            m_pBrush
        );

        // 7. Індикатор активності у правому верхньому куті
        m_pBrush->SetColor(COL_EMERALD);
        m_activeRT->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(x + size * 0.80f, y + size * 0.20f), size * 0.045f, size * 0.045f),
            m_pBrush
        );
    }

    void DrawMainStudioScene(float W, float H) {
        const float pad = 16.0f;
        const float leftW = 305.0f;
        const float rightW = 335.0f;
        const float topBarH = 56.0f;

        // ====================================================================
        // 1. ВЕРХНІЙ ЗАГОЛОВОК СТУДІЇ (Header Bar + Підтримка автора + GitHub)
        // ====================================================================
        UiRect headerRect{pad, pad, W - pad, pad + topBarH};
        DrawRoundedCard(headerRect, COL_BG_PANEL, COL_BORDER, 12.0f);

        DrawCyberEyeLogo(pad + 14.0f, pad + 9.0f, 38.0f);
        DrawTextStr(L"EYECAM STUDIO", {pad + 62, pad, pad + 245, pad + topBarH}, m_pFontTitle, COL_TEXT_PRIMARY);
        DrawTextStr(L"v1.4.0 • 4K DirectShow & Pro ISP", {pad + 225, pad + 2, pad + 470, pad + topBarH}, m_pFontBody, COL_TEXT_SECOND);

        // Індикатор статусу драйвера DirectShow праворуч у шапці
        UiRect drvBadge{W - pad - 246, pad + 11, W - pad - 14, pad + topBarH - 11};
        DrawRoundedCard(drvBadge, m_state.isDriverRegistered ? HexColor(0x092E20) : HexColor(0x332409),
                        m_state.isDriverRegistered ? COL_EMERALD : COL_AMBER, 16.0f);
        DrawTextStr(
            m_state.isDriverRegistered
                ? L"● DirectShow Webcam: ACTIVE"
                : TR(L"▲ DirectShow: NOT REGISTERED", L"▲ DirectShow: НЕ ЗАРЕЄСТРОВАНО"),
            drvBadge, m_pFontMono, m_state.isDriverRegistered ? COL_EMERALD : COL_AMBER,
            DWRITE_TEXT_ALIGNMENT_CENTER
        );

        // Кнопка «🐙 GitHub / Оновлення» у шапці
        bool hasUpd = m_state.updateAvailable.load();
        m_rcBtnHeaderGithub = {drvBadge.left - 178.0f, pad + 11, drvBadge.left - 10.0f, pad + topBarH - 11};
        DrawRoundedCard(
            m_rcBtnHeaderGithub,
            hasUpd ? HexColor(0x0B2E26) : COL_BG_CARD,
            hasUpd ? COL_EMERALD : COL_CYAN,
            10.0f
        );
        DrawTextStr(
            hasUpd ? TR(L"⬆ GitHub Update!", L"⬆ Оновлення GitHub!") : TR(L"🐙 GitHub • Updates", L"🐙 GitHub • Оновлення"),
            m_rcBtnHeaderGithub, m_pFontBody,
            hasUpd ? COL_EMERALD : COL_CYAN,
            DWRITE_TEXT_ALIGNMENT_CENTER
        );

        // Кнопка «❤️ Підтримати автора» у шапці
        m_rcBtnHeaderSupport = {m_rcBtnHeaderGithub.left - 166.0f, pad + 11, m_rcBtnHeaderGithub.left - 10.0f, pad + topBarH - 11};
        DrawRoundedCard(m_rcBtnHeaderSupport, HexColor(0x2A121B), COL_PATREON, 10.0f);
        DrawTextStr(
            TR(L"❤️ Support Author", L"❤️ Підтримати автора"),
            m_rcBtnHeaderSupport, m_pFontBody, HexColor(0xFF8A92),
            DWRITE_TEXT_ALIGNMENT_CENTER
        );

        // Кнопка перемикання мови (EN за замовчуванням / UA)
        m_rcBtnHeaderLang = {m_rcBtnHeaderSupport.left - 96.0f, pad + 11, m_rcBtnHeaderSupport.left - 10.0f, pad + topBarH - 11};
        DrawRoundedCard(m_rcBtnHeaderLang, HexColor(0x132436), COL_CYAN, 10.0f);
        DrawTextStr(
            m_state.isUkrainian ? L"🌐 Мова: UA" : L"🌐 Lang: EN",
            m_rcBtnHeaderLang, m_pFontBody, COL_CYAN,
            DWRITE_TEXT_ALIGNMENT_CENTER
        );

        float contentTop = headerRect.bottom + pad;
        float contentBottom = H - pad;

        // ====================================================================
        // 2. ЛІВА ПАНЕЛЬ: Підключення (Wi-Fi LAN або Випадаючий список USB)
        // ====================================================================
        UiRect leftPanel{pad, contentTop, pad + leftW, contentBottom};
        DrawRoundedCard(leftPanel, COL_BG_PANEL, COL_BORDER, 12.0f);

        float lx = leftPanel.left + 16;
        float rx = leftPanel.right - 16;
        float cy = leftPanel.top + 16;

        DrawTextStr(TR(L"1. CONNECTION TRANSPORT", L"1. СПОСІБ ПІДКЛЮЧЕННЯ"), {lx, cy, rx, cy + 24}, m_pFontSection, COL_TEXT_PRIMARY);
        cy += 30;

        float tabW = (rx - lx - 8.0f) * 0.5f;
        const wchar_t* tabNames[2] = {L"Wi-Fi (LAN)", TR(L"USB (Cable)", L"USB (Кабель)")};
        for (int i = 0; i < 2; ++i) {
            m_rcTransportTabs[i] = {lx + i * (tabW + 8.0f), cy, lx + i * (tabW + 8.0f) + tabW, cy + 36.0f};
            bool active = (static_cast<int>(m_state.transport) == i);
            DrawRoundedCard(m_rcTransportTabs[i], active ? COL_CYAN : COL_BG_CARD, active ? COL_CYAN : COL_BORDER, 8.0f);
            DrawTextStr(tabNames[i], m_rcTransportTabs[i], m_pFontSection,
                        active ? HexColor(0x05080D) : COL_TEXT_PRIMARY, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        cy += 48;

        if (m_state.transport == TransportTab::WIFI_LAN) {
            DrawTextStr(TR(L"Smartphone IP address (see mobile app):", L"IP-адреса смартфона (див. у додатку):"), {lx, cy, rx, cy + 20}, m_pFontBody, COL_TEXT_SECOND);
            cy += 24;
            m_rcIpInput = {lx, cy, rx, cy + 38};
            m_rcUsbDropdown = {0, 0, 0, 0};
            m_rcBtnRefreshUsb = {0, 0, 0, 0};

            DrawRoundedCard(m_rcIpInput, HexColor(0x0D1117), m_state.ipInputFocused ? COL_CYAN : COL_BORDER, 8.0f);
            std::wstring ipW(m_state.deviceIp.begin(), m_state.deviceIp.end());
            if (m_state.ipInputFocused && ((m_frameTick / 15) % 2 == 0)) ipW += L"|";
            DrawTextStr(L"  " + ipW, m_rcIpInput, m_pFontMono, COL_TEXT_PRIMARY);
            cy += 46;

            DrawTextStr(TR(L"Video & Control Port (TCP): 8554", L"Порт синхронізації та відео (TCP): 8554"), {lx, cy, rx, cy + 20}, m_pFontBody, COL_TEXT_DIM);
            cy += 30;
        } else {
            m_rcIpInput = {0, 0, 0, 0};
            DrawTextStr(TR(L"Select USB-connected smartphone:", L"Виберіть смартфон, підключений по USB:"), {lx, cy, rx, cy + 20}, m_pFontBody, COL_TEXT_SECOND);
            cy += 24;

            m_rcUsbDropdown = {lx, cy, rx, cy + 38};
            DrawRoundedCard(m_rcUsbDropdown, HexColor(0x0D1117), m_state.usbDropdownOpen ? COL_CYAN : COL_BORDER, 8.0f);

            std::wstring selectedUsbLabel = TR(L"  No USB devices detected", L"  Пристроїв USB не виявлено");
            {
                std::lock_guard<std::mutex> lock(m_usbMutex);
                if (!m_state.usbDevices.empty()) {
                    int idx = std::clamp(m_state.selectedUsbIdx, 0, static_cast<int>(m_state.usbDevices.size()) - 1);
                    selectedUsbLabel = L"  " + m_state.usbDevices[idx].displayName;
                }
            }
            DrawTextStr(selectedUsbLabel, {lx, cy, rx - 28, cy + 38}, m_pFontBody,
                        m_state.usbDevices.empty() ? COL_AMBER : COL_EMERALD);
            DrawTextStr(m_state.usbDropdownOpen ? L"▲" : L"▼", {rx - 28, cy, rx - 6, cy + 38}, m_pFontBody, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
            cy += 44;

            m_rcBtnRefreshUsb = {lx, cy, rx, cy + 32};
            DrawRoundedCard(m_rcBtnRefreshUsb, COL_BG_CARD, COL_BORDER, 8.0f);
            DrawTextStr(TR(L"↻  Refresh USB Device List", L"↻  Оновити список USB-пристроїв"), m_rcBtnRefreshUsb, m_pFontBody, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
            cy += 40;
        }

        // Головна кнопка СТАРТ / СТОП трансляції
        m_rcBtnStartStop = {lx, cy, rx, cy + 44};
        DrawRoundedCard(
            m_rcBtnStartStop,
            m_state.isStreaming ? HexColor(0x3A121C) : COL_EMERALD,
            m_state.isStreaming ? COL_CRIMSON : COL_EMERALD,
            10.0f
        );
        DrawTextStr(
            m_state.isStreaming
                ? TR(L"■  STOP VIRTUAL CAMERA", L"■  ЗУПИНИТИ ВІРТУАЛЬНУ КАМЕРУ")
                : TR(L"▶  START STREAMING", L"▶  ЗАПУСТИТИ ТРАНСЛЯЦІЮ"),
            m_rcBtnStartStop, m_pFontSection,
            m_state.isStreaming ? COL_CRIMSON : HexColor(0x06120C),
            DWRITE_TEXT_ALIGNMENT_CENTER
        );
        cy += 58;

        // Блок реєстрації системного драйвера DirectShow
        DrawTextStr(TR(L"2. SYSTEM VIRTUAL CAMERA DRIVER", L"2. СИСТЕМНИЙ ДРАЙВЕР КАМЕРИ"), {lx, cy, rx, cy + 24}, m_pFontSection, COL_TEXT_PRIMARY);
        cy += 28;

        UiRect drvCard{lx, cy, rx, cy + 126};
        DrawRoundedCard(drvCard, COL_BG_CARD, COL_BORDER, 10.0f);
        DrawTextStr(TR(L"Device: EyeCam Virtual Camera", L"Камера: EyeCam Virtual Camera"), {lx + 12, cy + 8, rx - 12, cy + 30}, m_pFontBody, COL_TEXT_PRIMARY);
        DrawTextStr(
            m_state.isDriverRegistered
                ? TR(L"Status: Registered in Windows\nAvailable in OBS, Zoom, Discord, Teams", L"Статус: Зареєстровано в системі\nДоступна в OBS, Zoom, Discord, Teams")
                : TR(L"Status: DLL Registration Required\nClick button below (Admin UAC)", L"Статус: Потрібна реєстрація DLL\nНатисніть кнопку нижче (права Admin)"),
            {lx + 12, cy + 32, rx - 12, cy + 76}, m_pFontBody,
            m_state.isDriverRegistered ? COL_EMERALD : COL_AMBER
        );

        m_rcBtnRegisterDriver = {lx + 12, cy + 82, rx - 12, cy + 116};
        DrawRoundedCard(m_rcBtnRegisterDriver, COL_BG_PANEL, COL_CYAN, 8.0f);
        DrawTextStr(
            m_state.isDriverRegistered
                ? TR(L"↻ Re-register Driver (UAC)", L"↻ Перереєструвати драйвер (UAC)")
                : TR(L"⚡ Register Virtual Camera", L"⚡ Зареєструвати камеру в системі"),
            m_rcBtnRegisterDriver, m_pFontBody, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER
        );

        if (m_state.transport == TransportTab::USB_ADB && m_state.usbDropdownOpen) {
            std::lock_guard<std::mutex> lock(m_usbMutex);
            int count = static_cast<int>(m_state.usbDevices.size());
            int rows = (count > 0) ? count : 1;
            UiRect dropBox{m_rcUsbDropdown.left, m_rcUsbDropdown.bottom + 2, m_rcUsbDropdown.right, m_rcUsbDropdown.bottom + 2 + rows * 34.0f};
            DrawRoundedCard(dropBox, COL_BG_DROPDOWN, COL_CYAN, 8.0f);

            m_rcUsbItems.clear();
            if (count == 0) {
                DrawTextStr(TR(L"  No active USB ADB devices", L"  Немає активних USB ADB пристроїв"), dropBox, m_pFontBody, COL_TEXT_SECOND);
            } else {
                for (int i = 0; i < count; ++i) {
                    UiRect itemR{dropBox.left + 4, dropBox.top + i * 34.0f + 2, dropBox.right - 4, dropBox.top + (i + 1) * 34.0f - 2};
                    m_rcUsbItems.push_back(itemR);
                    if (i == m_state.selectedUsbIdx) {
                        DrawRoundedCard(itemR, HexColor(0x162C3D), COL_CYAN, 6.0f);
                    }
                    DrawTextStr(L"  " + m_state.usbDevices[i].displayName, itemR, m_pFontBody, COL_TEXT_PRIMARY);
                }
            }
        }

        // ====================================================================
        // 3. ЦЕНТРАЛЬНИЙ В'ЮПОРТ (16:9 Live Video + Точковий Фокус + Телеметрія)
        // ====================================================================
        UiRect centerPanel{leftPanel.right + pad, contentTop, W - pad - rightW - pad, contentBottom};
        DrawRoundedCard(centerPanel, COL_BG_PANEL, COL_BORDER, 12.0f);

        float vpMargin = 16.0f;
        float availW = centerPanel.Width() - vpMargin * 2;
        float availH = centerPanel.Height() - vpMargin * 2 - 58.0f;
        float videoW = availW;
        float videoH = videoW * (9.0f / 16.0f);
        if (videoH > availH) {
            videoH = availH;
            videoW = videoH * (16.0f / 9.0f);
        }
        float vx = centerPanel.left + (centerPanel.Width() - videoW) * 0.5f;
        float vy = centerPanel.top + vpMargin + (availH - videoH) * 0.5f;
        m_rcVideoViewport = {vx, vy, vx + videoW, vy + videoH};
        const UiRect& videoRect = m_rcVideoViewport;

        bool hasFrame = false;
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            hasFrame = m_hasLiveFrame && !m_previewPixels.empty();
            if (hasFrame && m_pPreviewBitmap) {
                D2D1_RECT_U copyRect = D2D1::RectU(0, 0, 640, 360);
                m_pPreviewBitmap->CopyFromMemory(&copyRect, m_previewPixels.data(), 640 * 4);
            }
        }

        if (m_state.isStreaming && m_state.isPhoneConnected && hasFrame && m_pPreviewBitmap) {
            m_activeRT->DrawBitmap(m_pPreviewBitmap, videoRect.ToD2D(), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            DrawRoundedCard(videoRect, HexColor(0x070A0F), COL_BORDER, 8.0f);

            float midY = (videoRect.top + videoRect.bottom) * 0.5f;
            DrawCyberEyeLogo((videoRect.left + videoRect.right) * 0.5f - 24.0f, midY - 72.0f, 48.0f);

            if (!m_state.isStreaming) {
                DrawTextStr(TR(L"STREAM PAUSED", L"ТРАНСЛЯЦІЮ ПРИЗУПИНЕНО"), {videoRect.left, midY - 12, videoRect.right, midY + 18},
                            m_pFontTitle, COL_TEXT_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (m_state.transport == TransportTab::USB_ADB) {
                DrawTextStr(TR(L"WAITING FOR VIDEO STREAM OVER USB (ADB)...", L"ОЧІКУВАННЯ ВІДЕОПОТОКУ ПО USB (ADB)..."), {videoRect.left, midY - 14, videoRect.right, midY + 14},
                            m_pFontSection, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
                DrawTextStr(
                    TR(L"1. Connect phone via USB cable and enable 'USB Debugging'\n"
                       L"2. Open EyeCam on your smartphone — streaming starts automatically",
                       L"1. Підключіть телефон кабелем USB та увімкніть «Налагодження по USB»\n"
                       L"2. Відкрийте EyeCam на смартфоні — трансляція запуститься автоматично"),
                    {videoRect.left + 20, midY + 20, videoRect.right - 20, midY + 72},
                    m_pFontBody, COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER
                );
            } else {
                std::wstring waitWifi = (m_state.isUkrainian ? L"ОЧІКУВАННЯ ПІДКЛЮЧЕННЯ ДО " : L"WAITING FOR CONNECTION TO ") + Utf8ToWide(m_state.deviceIp) + L":8554...";
                DrawTextStr(waitWifi, {videoRect.left, midY - 14, videoRect.right, midY + 14},
                            m_pFontSection, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
                DrawTextStr(
                    TR(L"Check the IP address in the top-left corner of the EyeCam mobile app\n"
                       L"and tap the green Play button on your phone.",
                       L"Перевірте IP-адресу у верхньому лівому куті додатку EyeCam на телефоні\n"
                       L"та натисніть зелену кнопку Play у мобільному застосунку."),
                    {videoRect.left + 20, midY + 20, videoRect.right - 20, midY + 72},
                    m_pFontBody, COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER
                );
            }
        }

        // Сітка композиції (Rule of Thirds)
        if (m_state.showGrid && m_state.isStreaming) {
            m_pBrush->SetColor(HexColor(0xFFFFFF, 0.16f));
            for (int i = 1; i <= 2; ++i) {
                float gx = videoRect.left + (videoRect.Width() * i) / 3.0f;
                float gy = videoRect.top + (videoRect.Height() * i) / 3.0f;
                m_activeRT->DrawLine(D2D1::Point2F(gx, videoRect.top), D2D1::Point2F(gx, videoRect.bottom), m_pBrush, 1.0f);
                m_activeRT->DrawLine(D2D1::Point2F(videoRect.left, gy), D2D1::Point2F(videoRect.right, gy), m_pBrush, 1.0f);
            }
        }

        // Візуальна рамка автофокусу у в'юпорті при натисканні «🎯 Автофокус» або кліку по відео
        if (m_frameTick < m_state.afPulseUntilTick) {
            float rem = static_cast<float>(m_state.afPulseUntilTick - m_frameTick) / 40.0f;
            float fx = videoRect.left + m_state.afTargetNormX * videoRect.Width();
            float fy = videoRect.top + m_state.afTargetNormY * videoRect.Height();
            float boxR = 26.0f + (1.0f - rem) * 8.0f;
            UiRect afBox{fx - boxR, fy - boxR, fx + boxR, fy + boxR};
            m_pBrush->SetColor(HexColor(0x00E5FF, std::clamp(rem * 1.2f, 0.25f, 0.95f)));
            m_activeRT->DrawRoundedRectangle(D2D1::RoundedRect(afBox.ToD2D(), 8.0f, 8.0f), m_pBrush, 2.2f);
            m_pBrush->SetColor(COL_EMERALD);
            m_activeRT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fx, fy), 3.5f, 3.5f), m_pBrush);
        }

        // Рамка в'юпорту
        m_pBrush->SetColor(m_state.isPhoneConnected ? COL_EMERALD : COL_BORDER);
        m_activeRT->DrawRoundedRectangle(D2D1::RoundedRect(videoRect.ToD2D(), 6.0f, 6.0f), m_pBrush, 1.5f);

        // Плаваючий бейдж формату, активного модуля та внутрішнього зуму поверх відео
        const wchar_t* modShortNames[5] = {L"0.5x UW", L"1x MAIN", L"TELE", L"5x/MC", L"FRONT"};
        int modIdx = std::clamp(m_state.activeModuleIdx, 0, 4);
        uint32_t liveFps = m_state.measuredFps.load();
        wchar_t badgeBuf[200]{};
        swprintf_s(
            badgeBuf,
            L" %s • %ux%u@%u • %ls (%.1fx)%s%s ",
            m_state.isPhoneConnected ? L"● LIVE" : L"○ STANDBY",
            m_state.width, m_state.height,
            (m_state.isPhoneConnected && liveFps > 0) ? liveFps : m_state.fps,
            modShortNames[modIdx],
            m_state.intraModuleZoom,
            m_state.aeLocked ? L" • 🔒AE" : L"",
            m_state.torchOn ? L" • 🔦ON" : L""
        );
        UiRect hudBadge1{videoRect.left + 12, videoRect.top + 12, videoRect.left + 390, videoRect.top + 38};
        DrawRoundedCard(hudBadge1, HexColor(0x0B0E14, 0.86f), m_state.isPhoneConnected ? COL_EMERALD : COL_BORDER, 6.0f);
        DrawTextStr(badgeBuf, hudBadge1, m_pFontMono, m_state.isPhoneConnected ? COL_EMERALD : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);

        // Нижній телеметричний бар під відео
        float barY = centerPanel.bottom - 52.0f;
        UiRect teleBar{centerPanel.left + 16, barY, centerPanel.right - 16, centerPanel.bottom - 14};
        DrawRoundedCard(teleBar, COL_BG_CARD, COL_BORDER, 8.0f);

        std::wstring phoneNameCopy;
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            phoneNameCopy = m_state.connectedPhoneName;
        }
        wchar_t teleStr[280]{};
        if (m_state.isPhoneConnected && m_state.phoneThermalC > 1.0f) {
            swprintf_s(
                teleStr,
                m_state.isUkrainian
                    ? L"  %ls   |   %u FPS   |   %.1f Мбіт/с   |   %.0f°C • %d%%   |   Кадрів: %llu"
                    : L"  %ls   |   %u FPS   |   %.1f Mbps   |   %.0f°C • %d%%   |   Frames: %llu",
                phoneNameCopy.c_str(),
                liveFps,
                m_state.measuredBitrateKbps.load() / 1000.0f,
                m_state.phoneThermalC,
                m_state.phoneBatteryPct,
                static_cast<unsigned long long>(m_state.rxFrames.load())
            );
        } else {
            swprintf_s(
                teleStr,
                m_state.isUkrainian
                    ? L"  Пристрій: %ls   |   %u FPS   |   %.1f Мбіт/с   |   Кадрів: %llu"
                    : L"  Device: %ls   |   %u FPS   |   %.1f Mbps   |   Frames: %llu",
                phoneNameCopy.c_str(),
                liveFps,
                m_state.measuredBitrateKbps.load() / 1000.0f,
                static_cast<unsigned long long>(m_state.rxFrames.load())
            );
        }
        DrawTextStr(teleStr, teleBar, m_pFontMono, COL_TEXT_SECOND);

        // ====================================================================
        // 4. ПРАВА ПАНЕЛЬ: Окремі Модулі + Внутрішній Зум, Формат, ISP Замочки, LUT
        // ====================================================================
        UiRect rightPanel{W - pad - rightW, contentTop, W - pad, contentBottom};
        DrawRoundedCard(rightPanel, COL_BG_PANEL, COL_BORDER, 12.0f);

        float rlx = rightPanel.left + 16;
        float rrx = rightPanel.right - 16;
        float rcy = rightPanel.top + 14;

        // 3. Окремі кнопки фізичних модулів (активні лише за наявності в пристрої)
        DrawTextStr(TR(L"3. CAMERA MODULES & INTRA-ZOOM", L"3. МОДУЛІ КАМЕРИ ТА ВНУТРІШНІЙ ЗУМ"), {rlx, rcy, rrx, rcy + 20}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 23;

        const wchar_t* moduleBtnLabels[5] = {L"0.5x UW", L"1x Main", L"Tele", L"5x/MC", L"Front"};
        float modGap = 5.0f;
        float modW = (rrx - rlx - modGap * 4.0f) / 5.0f;

        for (int i = 0; i < 5; ++i) {
            m_rcModuleBtns[i] = {rlx + i * (modW + modGap), rcy, rlx + i * (modW + modGap) + modW, rcy + 30.0f};
            bool avail = m_state.moduleAvailable[i];
            bool sel = avail && (m_state.activeModuleIdx == i);
            D2D1_COLOR_F fillCol = !avail ? HexColor(0x0E1219) : (sel ? COL_EMERALD : COL_BG_CARD);
            D2D1_COLOR_F brdCol  = !avail ? HexColor(0x1B2230) : (sel ? COL_EMERALD : COL_BORDER);
            D2D1_COLOR_F txtCol  = !avail ? COL_TEXT_DIM : (sel ? HexColor(0x06120C) : COL_TEXT_PRIMARY);

            DrawRoundedCard(m_rcModuleBtns[i], fillCol, brdCol, 7.0f);
            DrawTextStr(moduleBtnLabels[i], m_rcModuleBtns[i], m_pFontBody, txtCol, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        rcy += 35;

        // Повзунок зуму суворо всередині обраного модуля (без авто-перемикання об'єктивів)
        wchar_t zoomSliderText[120]{};
        swprintf_s(
            zoomSliderText,
            m_state.isUkrainian ? L"Зум всередині модуля: %.1fx  (1.0x – %.1fx)" : L"Intra-Module Zoom: %.1fx  (1.0x – %.1fx)",
            m_state.intraModuleZoom, m_state.maxIntraModuleZoom
        );
        DrawTextStr(zoomSliderText, {rlx, rcy, rrx, rcy + 16}, m_pFontBody, COL_CYAN);
        rcy += 18;

        m_rcZoomSlider = {rlx, rcy, rrx, rcy + 16};
        UiRect zTrack{rlx, rcy + 5, rrx, rcy + 11};
        DrawRoundedCard(zTrack, COL_BG_CARD, COL_BORDER, 3.0f);
        float zFillW = zTrack.Width() * m_state.zoomSliderNorm;
        DrawRoundedCard({rlx, rcy + 5, rlx + zFillW, rcy + 11}, COL_EMERALD, COL_EMERALD, 3.0f);
        m_pBrush->SetColor(COL_TEXT_PRIMARY);
        m_activeRT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rlx + zFillW, rcy + 8.0f), 6.5f, 6.5f), m_pBrush);
        rcy += 22;

        // 4. Формат віртуальної камери (адаптується до можливостей смартфона, напр. Redmi 8A -> 30 FPS)
        DrawTextStr(TR(L"4. VIDEO RESOLUTION", L"4. РОЗДІЛЬНА ЗДАТНІСТЬ ВІДЕО"), {rlx, rcy, rrx, rcy + 20}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 23;

        const wchar_t* resLabels60[4] = {L"4K UHD", L"2K QHD", L"1080p 60", L"720p 60"};
        const wchar_t* resLabels30[4] = {L"4K UHD", L"2K QHD", L"1080p 30", L"720p 30"};
        float resW = (rrx - rlx - 18.0f) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            m_rcResBtns[i] = {rlx + i * (resW + 6.0f), rcy, rlx + i * (resW + 6.0f) + resW, rcy + 30.0f};
            bool resSupported = true;
            if (i == 0 && !m_state.phoneSupports4K) resSupported = false;
            if (i == 1 && !m_state.phoneSupports2K) resSupported = false;
            bool sel = resSupported && (m_state.resPresetIdx == i);
            D2D1_COLOR_F fillCol = !resSupported ? HexColor(0x0E1219) : (sel ? COL_CYAN : COL_BG_CARD);
            D2D1_COLOR_F brdCol  = !resSupported ? HexColor(0x1B2230) : (sel ? COL_CYAN : COL_BORDER);
            D2D1_COLOR_F txtCol  = !resSupported ? COL_TEXT_DIM : (sel ? HexColor(0x05080D) : COL_TEXT_PRIMARY);
            DrawRoundedCard(m_rcResBtns[i], fillCol, brdCol, 8.0f);
            DrawTextStr(
                m_state.phoneSupports60Fps ? resLabels60[i] : resLabels30[i],
                m_rcResBtns[i], m_pFontBody, txtCol, DWRITE_TEXT_ALIGNMENT_CENTER
            );
        }
        rcy += 37;

        // 5. Експозиція, Фокус, Ліхтарик та Замочки повзунків (ISP)
        DrawTextStr(TR(L"5. EXPOSURE, FOCUS & LIGHT (ISP)", L"5. ЕКСПОЗИЦІЯ, ФОКУС ТА СВІТЛО (ISP)"), {rlx, rcy, rrx, rcy + 20}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 23;

        bool anyManualSlider = m_state.sliderManual[0] || m_state.sliderManual[1] ||
                               m_state.sliderManual[2] || m_state.sliderManual[3];
        bool autoPulseActive = (m_frameTick < m_state.autoPulseUntilTick);
        bool afPulseActive = (m_frameTick < m_state.afPulseUntilTick);

        float halfW = (rrx - rlx - 6.0f) * 0.5f;
        m_rcBtnModeToggle = {rlx, rcy, rlx + halfW, rcy + 30.0f};
        m_rcBtnRefocus    = {rlx + halfW + 6.0f, rcy, rrx, rcy + 30.0f};

        // Кнопка «✨ Все в AUTO» із чітким візуальним відгуком при натисканні
        D2D1_COLOR_F autoFill = autoPulseActive
            ? COL_EMERALD
            : (!anyManualSlider ? HexColor(0x0C2A1E) : COL_BG_CARD);
        D2D1_COLOR_F autoBrd = (autoPulseActive || !anyManualSlider) ? COL_EMERALD : COL_BORDER;
        D2D1_COLOR_F autoTxt = autoPulseActive
            ? HexColor(0x04100A)
            : (!anyManualSlider ? COL_EMERALD : COL_TEXT_PRIMARY);
        DrawRoundedCard(m_rcBtnModeToggle, autoFill, autoBrd, 8.0f);
        DrawTextStr(
            autoPulseActive
                ? TR(L"✓ AUTO APPLIED!", L"✓ AUTO АКТИВОВАНО!")
                : (!anyManualSlider ? TR(L"✨ Mode: ALL AUTO", L"✨ Режим: ВСЕ В AUTO") : TR(L"✨ Reset All to AUTO", L"✨ Скинути все в AUTO")),
            m_rcBtnModeToggle, m_pFontBody, autoTxt, DWRITE_TEXT_ALIGNMENT_CENTER
        );

        // Кнопка «🎯 Автофокус» із чітким візуальним відгуком при натисканні
        D2D1_COLOR_F afFill = afPulseActive ? COL_CYAN : HexColor(0x162436);
        D2D1_COLOR_F afTxt  = afPulseActive ? HexColor(0x040A10) : COL_CYAN;
        DrawRoundedCard(m_rcBtnRefocus, afFill, COL_CYAN, 8.0f);
        DrawTextStr(
            afPulseActive ? TR(L"🎯 FOCUSING...", L"🎯 ФОКУСУВАННЯ...") : TR(L"🎯 Auto-Focus", L"🎯 Автофокус"),
            m_rcBtnRefocus, m_pFontBody, afTxt, DWRITE_TEXT_ALIGNMENT_CENTER
        );
        rcy += 35;

        // Другий ряд швидких кнопок: Блокування експозиції (AE Lock) + Ліхтарик (недійсний на фронталці)
        m_rcBtnAeLock = {rlx, rcy, rlx + halfW, rcy + 30.0f};
        m_rcBtnTorch  = {rlx + halfW + 6.0f, rcy, rrx, rcy + 30.0f};

        DrawRoundedCard(
            m_rcBtnAeLock,
            m_state.aeLocked ? HexColor(0x362709) : COL_BG_CARD,
            m_state.aeLocked ? COL_AMBER : COL_BORDER,
            8.0f
        );
        DrawTextStr(
            m_state.aeLocked ? TR(L"🔒 AE: LOCKED", L"🔒 Експоз.: ЗАФІКСОВАНО") : TR(L"🔓 Lock Exposure (AE)", L"🔓 Фіксація експозиції"),
            m_rcBtnAeLock, m_pFontBody,
            m_state.aeLocked ? COL_AMBER : COL_TEXT_SECOND,
            DWRITE_TEXT_ALIGNMENT_CENTER
        );

        bool isFrontActive = (m_state.activeModuleIdx == 4);
        bool torchAllowed = m_state.torchAvailable && !isFrontActive;
        D2D1_COLOR_F torchFill = !torchAllowed
            ? HexColor(0x0E1219)
            : (m_state.torchOn ? HexColor(0x382808) : COL_BG_CARD);
        D2D1_COLOR_F torchBrd = !torchAllowed
            ? HexColor(0x1B2230)
            : (m_state.torchOn ? COL_AMBER : COL_BORDER);
        D2D1_COLOR_F torchTxt = !torchAllowed
            ? COL_TEXT_DIM
            : (m_state.torchOn ? COL_AMBER : COL_TEXT_SECOND);
        DrawRoundedCard(m_rcBtnTorch, torchFill, torchBrd, 8.0f);
        DrawTextStr(
            isFrontActive
                ? TR(L"🔦 Torch (Front N/A)", L"🔦 Ліхтарик (Фронт.)")
                : (m_state.torchOn ? TR(L"🔦 Torch: ON", L"🔦 Ліхтарик: УВІМК.") : TR(L"🔦 Torch: OFF", L"🔦 Ліхтарик: ВИМК.")),
            m_rcBtnTorch, m_pFontBody, torchTxt, DWRITE_TEXT_ALIGNMENT_CENTER
        );
        rcy += 36;

        // 4 Повзунки ISP (кожен АВТО за замовчуванням; при ручному прокручуванні з'являється замочок 🔒)
        int isoVal = 100 + static_cast<int>(m_state.isoNorm * 3100.0f);
        int shutterDenom = 30 + static_cast<int>(m_state.shutterNorm * 3970.0f);
        int wbKelvin = 2800 + static_cast<int>(m_state.wbNorm * 5700.0f);

        wchar_t sBuf0[80], sBuf1[80], sBuf2[80], sBuf3[80];
        if (m_state.sliderManual[0]) swprintf_s(sBuf0, m_state.isUkrainian ? L"ISO Чутливість: %d (РУЧНИЙ)" : L"ISO Sensitivity: %d (MANUAL)", isoVal);
        else                         swprintf_s(sBuf0, m_state.isUkrainian ? L"ISO Чутливість: АВТО" : L"ISO Sensitivity: AUTO");

        if (m_state.sliderManual[1]) swprintf_s(sBuf1, m_state.isUkrainian ? L"Витримка (Shutter): 1/%d s (РУЧНИЙ)" : L"Shutter Speed: 1/%d s (MANUAL)", shutterDenom);
        else                         swprintf_s(sBuf1, m_state.isUkrainian ? L"Витримка (Shutter): АВТО" : L"Shutter Speed: AUTO");

        if (m_state.sliderManual[2]) swprintf_s(sBuf2, m_state.isUkrainian ? L"Фокусна відстань: %.2f dpt (РУЧНИЙ)" : L"Focus Distance: %.2f dpt (MANUAL)", m_state.focusNorm * 10.0f);
        else                         swprintf_s(sBuf2, m_state.isUkrainian ? L"Фокусна відстань: АВТО" : L"Focus Distance: AUTO");

        if (m_state.sliderManual[3]) swprintf_s(sBuf3, m_state.isUkrainian ? L"Баланс білого: %d K (РУЧНИЙ)" : L"White Balance: %d K (MANUAL)", wbKelvin);
        else                         swprintf_s(sBuf3, m_state.isUkrainian ? L"Баланс білого: АВТО" : L"White Balance: AUTO");

        const wchar_t* sliderTitles[4] = {sBuf0, sBuf1, sBuf2, sBuf3};
        float sliderVals[4] = {m_state.isoNorm, m_state.shutterNorm, m_state.focusNorm, m_state.wbNorm};

        for (int i = 0; i < 4; ++i) {
            bool isMan = m_state.sliderManual[i];
            DrawTextStr(sliderTitles[i], {rlx, rcy, rrx, rcy + 15}, m_pFontBody,
                        isMan ? COL_CYAN : COL_EMERALD);
            rcy += 16;

            float sliderRight = isMan ? (rrx - 32.0f) : rrx;
            m_rcSliders[i] = {rlx, rcy, sliderRight, rcy + 16};

            UiRect track{rlx, rcy + 5, sliderRight, rcy + 11};
            DrawRoundedCard(track, COL_BG_CARD, COL_BORDER, 3.0f);

            float fillW = track.Width() * sliderVals[i];
            UiRect fillRect{rlx, rcy + 5, rlx + fillW, rcy + 11};
            DrawRoundedCard(fillRect, isMan ? COL_CYAN : COL_TEXT_DIM,
                            isMan ? COL_CYAN : COL_TEXT_DIM, 3.0f);

            m_pBrush->SetColor(isMan ? COL_TEXT_PRIMARY : COL_TEXT_SECOND);
            m_activeRT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rlx + fillW, rcy + 8.0f), 6.0f, 6.0f), m_pBrush);

            if (isMan) {
                // Замочок 🔒 праворуч від повзунка — клік повертає цей повзунок в AUTO
                m_rcSliderLocks[i] = {rrx - 26.0f, rcy - 3.0f, rrx, rcy + 18.0f};
                DrawRoundedCard(m_rcSliderLocks[i], HexColor(0x362709), COL_AMBER, 5.0f);
                DrawTextStr(L"🔒", m_rcSliderLocks[i], m_pFontBody, COL_AMBER, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                m_rcSliderLocks[i] = {0, 0, 0, 0};
            }

            rcy += 20;
        }

        // 6. Завантаження кольорових профілів .cube 3D LUT та трансформація кадру
        rcy += 4;
        bool hasLut = m_cubeLut.IsLoaded();
        float lutBtnRight = hasLut ? (rrx - 34.0f) : rrx;
        m_rcBtnLoadLut = {rlx, rcy, lutBtnRight, rcy + 28.0f};
        DrawRoundedCard(
            m_rcBtnLoadLut,
            hasLut ? HexColor(0x132A38) : COL_BG_CARD,
            hasLut ? COL_CYAN : COL_BORDER,
            7.0f
        );
        std::wstring lutLabel = hasLut
            ? (L"🎨 .cube LUT: " + m_state.activeLutName)
            : TR(L"📂 Load .cube 3D LUT...", L"📂 Завантажити .cube 3D LUT...");
        DrawTextStr(lutLabel, m_rcBtnLoadLut, m_pFontBody, hasLut ? COL_CYAN : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);

        if (hasLut) {
            m_rcBtnClearLut = {rrx - 28.0f, rcy, rrx, rcy + 28.0f};
            DrawRoundedCard(m_rcBtnClearLut, HexColor(0x38141E), COL_CRIMSON, 7.0f);
            DrawTextStr(L"✕", m_rcBtnClearLut, m_pFontBody, COL_CRIMSON, DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            m_rcBtnClearLut = {0, 0, 0, 0};
        }
        rcy += 34;

        float btnW3 = (rrx - rlx - 12.0f) / 3.0f;
        m_rcBtnMirror = {rlx, rcy, rlx + btnW3, rcy + 30};
        m_rcBtnGrid   = {rlx + btnW3 + 6.0f, rcy, rlx + 2.0f * btnW3 + 6.0f, rcy + 30};
        m_rcBtnRotate = {rlx + 2.0f * (btnW3 + 6.0f), rcy, rrx, rcy + 30};

        DrawRoundedCard(m_rcBtnMirror, m_state.mirrorHorizontal ? HexColor(0x162C3D) : COL_BG_CARD,
                        m_state.mirrorHorizontal ? COL_CYAN : COL_BORDER, 8.0f);
        DrawTextStr(TR(L"⇄ Mirror", L"⇄ Дзеркало"), m_rcBtnMirror, m_pFontBody,
                    m_state.mirrorHorizontal ? COL_CYAN : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);

        DrawRoundedCard(m_rcBtnGrid, m_state.showGrid ? HexColor(0x162C3D) : COL_BG_CARD,
                        m_state.showGrid ? COL_CYAN : COL_BORDER, 8.0f);
        DrawTextStr(TR(L"# Grid 3x3", L"# Сітка 3x3"), m_rcBtnGrid, m_pFontBody,
                    m_state.showGrid ? COL_CYAN : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);

        wchar_t rotLabel[24]{};
        if (m_state.streamRotationDeg < 0) {
            swprintf_s(rotLabel, m_state.isUkrainian ? L"↻ Авто" : L"↻ Auto");
        } else {
            swprintf_s(rotLabel, L"↻ %d°", m_state.streamRotationDeg);
        }
        bool rotManual = (m_state.streamRotationDeg >= 0);
        DrawRoundedCard(m_rcBtnRotate, rotManual ? HexColor(0x162C3D) : COL_BG_CARD,
                        rotManual ? COL_CYAN : COL_BORDER, 8.0f);
        DrawTextStr(rotLabel, m_rcBtnRotate, m_pFontBody,
                    rotManual ? COL_CYAN : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Рендеринг модального вікна поверх розмитого (blurred) фону програми
    void DrawModalOverlayCard(float W, float H) {
        float cardW = 460.0f;
        float cardH = (m_state.activeModal == ModalOverlay::SUPPORT_AUTHOR) ? 280.0f : 310.0f;
        float cx = (W - cardW) * 0.5f;
        float cy = (H - cardH) * 0.5f;
        m_rcModalCard = {cx, cy, cx + cardW, cy + cardH};

        DrawRoundedCard(m_rcModalCard, HexColor(0x111722, 0.96f), COL_CYAN, 16.0f);

        m_rcModalClose = {m_rcModalCard.right - 42.0f, m_rcModalCard.top + 14.0f, m_rcModalCard.right - 14.0f, m_rcModalCard.top + 42.0f};
        DrawRoundedCard(m_rcModalClose, COL_BG_CARD, COL_BORDER, 8.0f);
        DrawTextStr(L"✕", m_rcModalClose, m_pFontSection, COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);

        if (m_state.activeModal == ModalOverlay::SUPPORT_AUTHOR) {
            DrawCyberEyeLogo(cx + 22.0f, cy + 18.0f, 38.0f);
            DrawTextStr(
                TR(L"Support EyeCam Author", L"Підтримати автора EyeCam"),
                {cx + 70.0f, cy + 16.0f, m_rcModalClose.left - 8.0f, cy + 44.0f},
                m_pFontTitle, COL_TEXT_PRIMARY
            );
            DrawTextStr(
                TR(L"Thank you for using EyeCam Studio! If this open-source project helps your workflow or streams, you can support its development:",
                   L"Дякую за використання EyeCam Studio! Якщо проєкт корисний у вашій роботі чи стрімах, ви можете підтримати його розвиток:"),
                {cx + 22.0f, cy + 62.0f, cx + cardW - 22.0f, cy + 112.0f},
                m_pFontBody, COL_TEXT_SECOND
            );

            // Кнопка 1: Patreon
            m_rcModalPrimaryBtn = {cx + 24.0f, cy + 126.0f, cx + cardW - 24.0f, cy + 174.0f};
            DrawRoundedCard(m_rcModalPrimaryBtn, COL_PATREON, HexColor(0xFF757D), 10.0f);
            DrawTextStr(
                TR(L"🧡  Support on Patreon  (Gladiator_Satan)", L"🧡  Підтримати на Patreon  (Gladiator_Satan)"),
                m_rcModalPrimaryBtn, m_pFontSection, HexColor(0xFFFFFF),
                DWRITE_TEXT_ALIGNMENT_CENTER
            );

            // Кнопка 2: Buy Me a Coffee
            m_rcModalSecondaryBtn = {cx + 24.0f, cy + 188.0f, cx + cardW - 24.0f, cy + 236.0f};
            DrawRoundedCard(m_rcModalSecondaryBtn, COL_BMAC, COL_AMBER, 10.0f);
            DrawTextStr(
                L"☕  Buy Me a Coffee  (gladiator_satan)",
                m_rcModalSecondaryBtn, m_pFontSection, HexColor(0x0B0E14),
                DWRITE_TEXT_ALIGNMENT_CENTER
            );

            DrawTextStr(
                TR(L"Click outside the card or press ✕ to return to Studio", L"Натисніть поза межами вікна або ✕, щоб повернутися до студії"),
                {cx + 20.0f, cy + 244.0f, cx + cardW - 20.0f, cy + 270.0f},
                m_pFontBody, COL_TEXT_DIM, DWRITE_TEXT_ALIGNMENT_CENTER
            );
        } else if (m_state.activeModal == ModalOverlay::GITHUB_UPDATE) {
            DrawCyberEyeLogo(cx + 22.0f, cy + 18.0f, 38.0f);
            DrawTextStr(
                TR(L"GitHub & Update Checker", L"GitHub & Перевірка оновлень"),
                {cx + 70.0f, cy + 16.0f, m_rcModalClose.left - 8.0f, cy + 44.0f},
                m_pFontTitle, COL_TEXT_PRIMARY
            );

            std::wstring statusCopy;
            std::wstring latestCopy;
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                statusCopy = m_state.updateStatusText;
                latestCopy = m_state.latestGithubTag;
            }
            bool hasUpd = m_state.updateAvailable.load();

            UiRect infoBox{cx + 24.0f, cy + 64.0f, cx + cardW - 24.0f, cy + 146.0f};
            DrawRoundedCard(infoBox, COL_BG_CARD, hasUpd ? COL_EMERALD : COL_BORDER, 10.0f);

            std::wstring verLine = (m_state.isUkrainian ? L"Поточна версія: v" : L"Installed: v") + std::wstring(kEyeCamVersionWide) +
                                   (m_state.isUkrainian ? L"   •   Останній реліз на GitHub: v" : L"   •   Latest GitHub Release: v") + latestCopy;
            DrawTextStr(verLine, {infoBox.left + 14.0f, infoBox.top + 10.0f, infoBox.right - 14.0f, infoBox.top + 38.0f},
                        m_pFontMono, COL_CYAN);
            DrawTextStr(statusCopy, {infoBox.left + 14.0f, infoBox.top + 42.0f, infoBox.right - 14.0f, infoBox.bottom - 10.0f},
                        m_pFontBody, hasUpd ? COL_EMERALD : COL_TEXT_PRIMARY);

            // Кнопка 1: Відкрити репозиторій GitHub
            m_rcModalPrimaryBtn = {cx + 24.0f, cy + 162.0f, cx + cardW - 24.0f, cy + 208.0f};
            DrawRoundedCard(m_rcModalPrimaryBtn, HexColor(0x182638), COL_CYAN, 10.0f);
            DrawTextStr(
                TR(L"🐙  Open EyeCam Repository on GitHub", L"🐙  Відкрити репозиторій EyeCam на GitHub"),
                m_rcModalPrimaryBtn, m_pFontSection, COL_CYAN,
                DWRITE_TEXT_ALIGNMENT_CENTER
            );

            // Кнопка 2: Перевірити оновлення або завантажити новий реліз
            m_rcModalSecondaryBtn = {cx + 24.0f, cy + 220.0f, cx + cardW - 24.0f, cy + 266.0f};
            DrawRoundedCard(
                m_rcModalSecondaryBtn,
                hasUpd ? COL_EMERALD : COL_BG_CARD,
                hasUpd ? COL_EMERALD : COL_BORDER,
                10.0f
            );
            DrawTextStr(
                hasUpd
                    ? TR(L"⬇  Download Latest Release from GitHub", L"⬇  Завантажити нову версію з GitHub Releases")
                    : TR(L"🔄  Check for Updates Now", L"🔄  Перевірити наявність оновлень зараз"),
                m_rcModalSecondaryBtn, m_pFontSection,
                hasUpd ? HexColor(0x05100A) : COL_TEXT_PRIMARY,
                DWRITE_TEXT_ALIGNMENT_CENTER
            );

            DrawTextStr(
                L"github.com/Gladiator666Satan/EyeCam",
                {cx + 20.0f, cy + 274.0f, cx + cardW - 20.0f, cy + 300.0f},
                m_pFontMono, COL_TEXT_DIM, DWRITE_TEXT_ALIGNMENT_CENTER
            );
        }
    }

    void OnPaint() {
        EnsureRenderTarget();
        if (!m_pRenderTarget) return;

        m_frameTick++;
        D2D1_SIZE_F sz = m_pRenderTarget->GetSize();
        float W = sz.width;
        float H = sz.height;

        bool modalOpen = (m_state.activeModal != ModalOverlay::NONE);
        if (modalOpen && EnsureBlurTargets(W, H)) {
            // 1. Малюємо головну сцену студії у повнорозмірний позаекранний буфер m_pSceneRT
            m_activeRT = m_pSceneRT;
            m_pSceneRT->BeginDraw();
            m_pSceneRT->Clear(COL_BG_MAIN);
            DrawMainStudioScene(W, H);
            m_pSceneRT->EndDraw();

            ID2D1Bitmap* pSceneBmp = nullptr;
            m_pSceneRT->GetBitmap(&pSceneBmp);

            // 2. Багатопрохідне Гаусове розмиття через даунсемплінг 1/6 із білінійною фільтрацією
            D2D1_SIZE_F smallSz = m_pBlurSmallRT->GetSize();
            D2D1_RECT_F smallRect = D2D1::RectF(0, 0, smallSz.width, smallSz.height);

            if (pSceneBmp) {
                m_pBlurSmallRT->BeginDraw();
                m_pBlurSmallRT->Clear(COL_BG_MAIN);
                m_pBlurSmallRT->DrawBitmap(pSceneBmp, smallRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                m_pBlurSmallRT->EndDraw();
                pSceneBmp->Release();
            }

            for (int pass = 0; pass < 2; ++pass) {
                ID2D1Bitmap* pSmallBmp = nullptr;
                if (SUCCEEDED(m_pBlurSmallRT->GetBitmap(&pSmallBmp)) && pSmallBmp) {
                    m_pBlurPingRT->BeginDraw();
                    m_pBlurPingRT->Clear(COL_BG_MAIN);
                    const float r = 1.6f + pass * 1.1f;
                    m_pBlurPingRT->DrawBitmap(pSmallBmp, D2D1::RectF(-r, 0, smallSz.width - r, smallSz.height), 0.25f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    m_pBlurPingRT->DrawBitmap(pSmallBmp, D2D1::RectF(r, 0, smallSz.width + r, smallSz.height), 0.25f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    m_pBlurPingRT->DrawBitmap(pSmallBmp, D2D1::RectF(0, -r, smallSz.width, smallSz.height - r), 0.25f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    m_pBlurPingRT->DrawBitmap(pSmallBmp, D2D1::RectF(0, r, smallSz.width, smallSz.height + r), 0.25f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    m_pBlurPingRT->EndDraw();
                    pSmallBmp->Release();
                }

                ID2D1Bitmap* pPingBmp = nullptr;
                if (SUCCEEDED(m_pBlurPingRT->GetBitmap(&pPingBmp)) && pPingBmp) {
                    m_pBlurSmallRT->BeginDraw();
                    m_pBlurSmallRT->Clear(COL_BG_MAIN);
                    const float d = 1.4f + pass * 1.0f;
                    m_pBlurSmallRT->DrawBitmap(pPingBmp, D2D1::RectF(-d, -d, smallSz.width - d, smallSz.height - d), 0.25f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    m_pBlurSmallRT->DrawBitmap(pPingBmp, D2D1::RectF(d, -d, smallSz.width + d, smallSz.height - d), 0.25f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    m_pBlurSmallRT->DrawBitmap(pPingBmp, D2D1::RectF(-d, d, smallSz.width - d, smallSz.height + d), 0.25f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    m_pBlurSmallRT->DrawBitmap(pPingBmp, D2D1::RectF(d, d, smallSz.width + d, smallSz.height + d), 0.25f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    m_pBlurSmallRT->EndDraw();
                    pPingBmp->Release();
                }
            }

            // 3. Виводимо розмитий фон на екран + напівпрозору темну вуаль + чітке модальне вікно
            m_activeRT = m_pRenderTarget;
            m_pRenderTarget->BeginDraw();
            m_pRenderTarget->Clear(COL_BG_MAIN);

            ID2D1Bitmap* pBlurredBmp = nullptr;
            if (SUCCEEDED(m_pBlurSmallRT->GetBitmap(&pBlurredBmp)) && pBlurredBmp) {
                m_pRenderTarget->DrawBitmap(
                    pBlurredBmp,
                    D2D1::RectF(0, 0, W, H),
                    1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
                );
                pBlurredBmp->Release();
            }

            m_pBrush->SetColor(HexColor(0x05080F, 0.72f));
            m_pRenderTarget->FillRectangle(D2D1::RectF(0, 0, W, H), m_pBrush);

            DrawModalOverlayCard(W, H);
        } else {
            m_activeRT = m_pRenderTarget;
            m_pRenderTarget->BeginDraw();
            m_pRenderTarget->Clear(COL_BG_MAIN);
            DrawMainStudioScene(W, H);
            if (modalOpen) {
                m_pBrush->SetColor(HexColor(0x05080F, 0.82f));
                m_pRenderTarget->FillRectangle(D2D1::RectF(0, 0, W, H), m_pBrush);
                DrawModalOverlayCard(W, H);
            }
        }

        HRESULT hr = m_pRenderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            ReleaseOffscreenBlurTargets();
            if (m_pPreviewBitmap) { m_pPreviewBitmap->Release(); m_pPreviewBitmap = nullptr; }
            if (m_pBrush) { m_pBrush->Release(); m_pBrush = nullptr; }
            if (m_pRenderTarget) { m_pRenderTarget->Release(); m_pRenderTarget = nullptr; }
        }
    }

    void UnlockSingleSliderToAuto(int idx) {
        if (idx < 0 || idx >= 4) return;
        m_state.sliderManual[idx] = false;
        if (idx == 0) {
            SendControlJson("{\"action\":\"SetISO\",\"auto\":true}", "SetISO");
        } else if (idx == 1) {
            SendControlJson("{\"action\":\"SetShutter\",\"auto\":true}", "SetShutter");
        } else if (idx == 2) {
            SendControlJson("{\"action\":\"SetFocus\",\"mode\":\"AUTO\"}", "SetFocus");
        } else if (idx == 3) {
            SendControlJson("{\"action\":\"SetWhiteBalance\",\"mode\":\"AUTO\"}", "SetWhiteBalance");
        }
    }

    void HandleMouseDown(float x, float y) {
        // Якщо відкрите модальне вікно (Підтримка автора або GitHub) — обробляємо кліки в ньому
        if (m_state.activeModal != ModalOverlay::NONE) {
            if (m_rcModalClose.Contains(x, y) || !m_rcModalCard.Contains(x, y)) {
                m_state.activeModal = ModalOverlay::NONE;
                return;
            }
            if (m_state.activeModal == ModalOverlay::SUPPORT_AUTHOR) {
                if (m_rcModalPrimaryBtn.Contains(x, y)) {
                    ShellExecuteW(m_hwnd, L"open", L"https://www.patreon.com/cw/Gladiator_Satan", nullptr, nullptr, SW_SHOWNORMAL);
                    return;
                }
                if (m_rcModalSecondaryBtn.Contains(x, y)) {
                    ShellExecuteW(m_hwnd, L"open", L"https://buymeacoffee.com/gladiator_satan", nullptr, nullptr, SW_SHOWNORMAL);
                    return;
                }
            } else if (m_state.activeModal == ModalOverlay::GITHUB_UPDATE) {
                if (m_rcModalPrimaryBtn.Contains(x, y)) {
                    ShellExecuteW(m_hwnd, L"open", L"https://github.com/Gladiator666Satan/EyeCam", nullptr, nullptr, SW_SHOWNORMAL);
                    return;
                }
                if (m_rcModalSecondaryBtn.Contains(x, y)) {
                    if (m_state.updateAvailable.load()) {
                        std::wstring url;
                        {
                            std::lock_guard<std::mutex> lock(m_frameMutex);
                            url = m_state.releaseHtmlUrl;
                        }
                        ShellExecuteW(m_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    } else {
                        CheckGithubUpdateAsync();
                    }
                    return;
                }
            }
            return;
        }

        // Кнопки у шапці: Перемикання мови (EN/UA), Підтримати автора та GitHub / Оновлення
        if (m_rcBtnHeaderLang.Contains(x, y)) {
            m_state.isUkrainian = !m_state.isUkrainian;
            SaveLanguagePref(m_state.isUkrainian);
            return;
        }
        if (m_rcBtnHeaderSupport.Contains(x, y)) {
            m_state.activeModal = ModalOverlay::SUPPORT_AUTHOR;
            return;
        }
        if (m_rcBtnHeaderGithub.Contains(x, y)) {
            m_state.activeModal = ModalOverlay::GITHUB_UPDATE;
            CheckGithubUpdateAsync();
            return;
        }

        // Якщо відкритий випадаючий список USB — спочатку перевіряємо вибір елемента списку
        if (m_state.transport == TransportTab::USB_ADB && m_state.usbDropdownOpen) {
            for (size_t i = 0; i < m_rcUsbItems.size(); ++i) {
                if (m_rcUsbItems[i].Contains(x, y)) {
                    {
                        std::lock_guard<std::mutex> lock(m_usbMutex);
                        if (i < m_state.usbDevices.size()) {
                            m_state.selectedUsbIdx = static_cast<int>(i);
                            m_state.selectedUsbSerial = m_state.usbDevices[i].serial;
                        }
                    }
                    m_state.usbDropdownOpen = false;
                    RefreshUsbDevicesAsync();
                    CloseActivePhoneSocket();
                    return;
                }
            }
            if (!m_rcUsbDropdown.Contains(x, y)) {
                m_state.usbDropdownOpen = false;
            }
        }

        m_state.ipInputFocused = (m_state.transport == TransportTab::WIFI_LAN) && m_rcIpInput.Contains(x, y);

        // Вкладки транспорту: Wi-Fi (LAN) або USB (Кабель)
        for (int i = 0; i < 2; ++i) {
            if (m_rcTransportTabs[i].Contains(x, y)) {
                m_state.transport = static_cast<TransportTab>(i);
                m_state.usbDropdownOpen = false;
                if (m_state.transport == TransportTab::USB_ADB) {
                    RefreshUsbDevicesAsync();
                }
                CloseActivePhoneSocket();
                return;
            }
        }

        if (m_state.transport == TransportTab::USB_ADB && m_rcUsbDropdown.Contains(x, y)) {
            m_state.usbDropdownOpen = !m_state.usbDropdownOpen;
            if (m_state.usbDropdownOpen) {
                RefreshUsbDevicesAsync();
            }
            return;
        }

        if (m_state.transport == TransportTab::USB_ADB && m_rcBtnRefreshUsb.Contains(x, y)) {
            RefreshUsbDevicesAsync();
            CloseActivePhoneSocket();
            return;
        }

        // Старт / Стоп трансляції
        if (m_rcBtnStartStop.Contains(x, y)) {
            m_state.isStreaming = !m_state.isStreaming;
            if (m_state.isStreaming) {
                std::lock_guard<std::mutex> lock(m_sinkMutex);
                m_sink.Open(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2, m_state.fps);
            } else {
                CloseActivePhoneSocket();
                std::lock_guard<std::mutex> lock(m_sinkMutex);
                m_sink.Close();
            }
            return;
        }

        // Реєстрація драйвера DirectShow
        if (m_rcBtnRegisterDriver.Contains(x, y)) {
            m_state.isDriverRegistered = RegisterDirectShowDriver(m_hwnd);
            return;
        }

        // Клік по відео-в'юпорту — точковий автофокус за координатами (x, y)
        if (m_rcVideoViewport.Contains(x, y) && m_rcVideoViewport.Width() > 20.0f && m_rcVideoViewport.Height() > 20.0f) {
            float nx = std::clamp((x - m_rcVideoViewport.left) / m_rcVideoViewport.Width(), 0.0f, 1.0f);
            float ny = std::clamp((y - m_rcVideoViewport.top) / m_rcVideoViewport.Height(), 0.0f, 1.0f);
            m_state.afTargetNormX = nx;
            m_state.afTargetNormY = ny;
            m_state.afPulseUntilTick = m_frameTick + 40;
            m_state.sliderManual[2] = false; // Розблоковуємо фокус в авто
            char json[128]{};
            std::snprintf(json, sizeof(json), "{\"action\":\"SetFocusPoint\",\"x\":%.3f,\"y\":%.3f}", nx, ny);
            SendControlJson(json);
            return;
        }

        // 3. Вибір фізичного модуля камери (активні лише наявні в смартфоні модулі)
        const char* moduleKeys[5] = {"uw", "main", "tele", "tele5x", "front"};
        for (int i = 0; i < 5; ++i) {
            if (m_rcModuleBtns[i].Contains(x, y)) {
                if (!m_state.moduleAvailable[i]) return;
                m_state.activeModuleIdx = i;
                m_state.intraModuleZoom = 1.0f;
                m_state.zoomSliderNorm = 0.0f;
                if (i == 4) {
                    m_state.torchOn = false; // На фронтальній камері ліхтарик вимикається
                }
                char json[128]{};
                std::snprintf(json, sizeof(json), "{\"action\":\"SetCameraModule\",\"module\":\"%s\"}", moduleKeys[i]);
                SendControlJson(json);
                return;
            }
        }

        // Повзунок зуму всередині поточного модуля
        if (m_rcZoomSlider.Contains(x, y)) {
            m_activeSliderDrag = 100; // 100 = Intra-Module Zoom Slider
            UpdateZoomSliderValue(x);
            return;
        }

        // 4. Вибір роздільної здатності (з урахуванням можливостей смартфона, напр. Redmi 8A -> 30 FPS)
        for (int i = 0; i < 4; ++i) {
            if (m_rcResBtns[i].Contains(x, y)) {
                if (i == 0 && !m_state.phoneSupports4K) return;
                if (i == 1 && !m_state.phoneSupports2K) return;

                uint32_t maxSupportedFps = m_state.phoneSupports60Fps ? 60u : 30u;
                uint32_t targetW = 1280, targetH = 720, targetFps = maxSupportedFps;
                if (i == 0)      { targetW = 3840; targetH = 2160; targetFps = 30; }
                else if (i == 1) { targetW = 2560; targetH = 1440; targetFps = maxSupportedFps; }
                else if (i == 2) { targetW = 1920; targetH = 1080; targetFps = maxSupportedFps; }

                {
                    std::lock_guard<std::mutex> lock(m_sinkMutex);
                    m_state.resPresetIdx = i;
                    m_state.width = targetW;
                    m_state.height = targetH;
                    m_state.fps = targetFps;
                    if (m_sink.IsOpen()) {
                        m_sink.SetFormat(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2, m_state.fps);
                    }
                }

                char json[160]{};
                std::snprintf(json, sizeof(json), "{\"action\":\"SetResolution\",\"width\":%u,\"height\":%u,\"fps\":%u}",
                              targetW, targetH, targetFps);
                SendControlJson(json);
                return;
            }
        }

        // 5. Кнопка «✨ Скинути все в AUTO» (з візуальним відгуком)
        if (m_rcBtnModeToggle.Contains(x, y)) {
            for (int i = 0; i < 4; ++i) m_state.sliderManual[i] = false;
            m_state.aeLocked = false;
            m_state.autoPulseUntilTick = m_frameTick + 38;
            SendControlJson("{\"action\":\"ResetAllToAuto\"}");
            return;
        }

        // Кнопка «🎯 Автофокус» (з візуальним відгуком на кнопці та рамкою у центрі відео)
        if (m_rcBtnRefocus.Contains(x, y)) {
            m_state.sliderManual[2] = false;
            m_state.afTargetNormX = 0.5f;
            m_state.afTargetNormY = 0.5f;
            m_state.afPulseUntilTick = m_frameTick + 40;
            SendControlJson("{\"action\":\"TriggerAutoFocus\"}");
            return;
        }

        // Кнопка блокування експозиції «🔒 Фіксація експозиції (AE Lock)»
        if (m_rcBtnAeLock.Contains(x, y)) {
            m_state.aeLocked = !m_state.aeLocked;
            char json[96]{};
            std::snprintf(json, sizeof(json), "{\"action\":\"SetAeLock\",\"locked\":%s}", m_state.aeLocked ? "true" : "false");
            SendControlJson(json);
            return;
        }

        // Кнопка увімкнення/вимкнення ліхтарика (недійсна на фронтальній камері)
        if (m_rcBtnTorch.Contains(x, y)) {
            if (m_state.activeModuleIdx == 4 || !m_state.torchAvailable) return;
            m_state.torchOn = !m_state.torchOn;
            char json[96]{};
            std::snprintf(json, sizeof(json), "{\"action\":\"SetTorch\",\"enabled\":%s}", m_state.torchOn ? "true" : "false");
            SendControlJson(json);
            return;
        }

        // Перевірка кліку на замочок 🔒 біля повзунка (повертає конкретний повзунок в AUTO)
        for (int i = 0; i < 4; ++i) {
            if (m_state.sliderManual[i] && m_rcSliderLocks[i].Contains(x, y)) {
                UnlockSingleSliderToAuto(i);
                return;
            }
        }

        // Перевірка натискання на повзунки ISP (переводить відповідний повзунок у ручний режим із замочком 🔒)
        for (int i = 0; i < 4; ++i) {
            if (m_rcSliders[i].Contains(x, y)) {
                m_state.sliderManual[i] = true;
                m_activeSliderDrag = i;
                UpdateSliderValue(i, x);
                return;
            }
        }

        // Кнопки .cube 3D LUT
        if (m_cubeLut.IsLoaded() && m_rcBtnClearLut.Contains(x, y)) {
            m_cubeLut.Clear();
            m_state.activeLutName.clear();
            return;
        }
        if (m_rcBtnLoadLut.Contains(x, y)) {
            ReleaseCapture();
            PromptAndLoadCubeLut();
            return;
        }

        // Перемикачі Дзеркало, Сітка та Поворот
        if (m_rcBtnMirror.Contains(x, y)) {
            m_state.mirrorHorizontal = !m_state.mirrorHorizontal;
            return;
        }
        if (m_rcBtnGrid.Contains(x, y)) {
            m_state.showGrid = !m_state.showGrid;
            return;
        }
        if (m_rcBtnRotate.Contains(x, y)) {
            int nextDeg = 0;
            if (m_state.streamRotationDeg < 0) nextDeg = 0;
            else if (m_state.streamRotationDeg == 0) nextDeg = 90;
            else if (m_state.streamRotationDeg == 90) nextDeg = 180;
            else if (m_state.streamRotationDeg == 180) nextDeg = 270;
            else nextDeg = -1;

            m_state.streamRotationDeg = nextDeg;
            char json[96]{};
            std::snprintf(json, sizeof(json), "{\"action\":\"SetRotation\",\"degrees\":%d}", nextDeg);
            SendControlJson(json, "SetRotation");
            return;
        }
    }

    void UpdateZoomSliderValue(float mouseX) {
        float norm = (mouseX - m_rcZoomSlider.left) / (std::max)(1.0f, m_rcZoomSlider.Width());
        norm = std::clamp(norm, 0.0f, 1.0f);
        m_state.zoomSliderNorm = norm;

        float zoom = 1.0f + norm * (std::max)(0.0f, m_state.maxIntraModuleZoom - 1.0f);
        m_state.intraModuleZoom = std::round(zoom * 10.0f) / 10.0f;

        char json[128]{};
        std::snprintf(json, sizeof(json), "{\"action\":\"SetZoom\",\"zoom\":%.2f}", m_state.intraModuleZoom);
        SendControlJson(json, "SetZoom");
    }

    void UpdateSliderValue(int idx, float mouseX) {
        if (idx < 0 || idx >= 4) return;
        const UiRect& r = m_rcSliders[idx];
        float norm = std::clamp((mouseX - r.left) / (std::max)(1.0f, r.Width()), 0.0f, 1.0f);
        m_state.sliderManual[idx] = true;

        char json[160]{};
        const char* coalesceKey = "";
        if (idx == 0) {
            m_state.isoNorm = norm;
            int iso = 100 + static_cast<int>(norm * 3100.0f);
            std::snprintf(json, sizeof(json), "{\"action\":\"SetISO\",\"auto\":false,\"iso\":%d}", iso);
            coalesceKey = "SetISO";
        } else if (idx == 1) {
            m_state.shutterNorm = norm;
            int denom = 30 + static_cast<int>(norm * 3970.0f);
            long long expNs = 1000000000LL / denom;
            std::snprintf(json, sizeof(json), "{\"action\":\"SetShutter\",\"auto\":false,\"exposure_ns\":%lld}", expNs);
            coalesceKey = "SetShutter";
        } else if (idx == 2) {
            m_state.focusNorm = norm;
            std::snprintf(json, sizeof(json), "{\"action\":\"SetFocus\",\"mode\":\"MANUAL\",\"distance_diopters\":%.2f}", norm * 10.0f);
            coalesceKey = "SetFocus";
        } else if (idx == 3) {
            m_state.wbNorm = norm;
            int kelvin = 2800 + static_cast<int>(norm * 5700.0f);
            std::snprintf(json, sizeof(json), "{\"action\":\"SetWhiteBalance\",\"mode\":\"MANUAL_KELVIN\",\"kelvin\":%d}", kelvin);
            coalesceKey = "SetWhiteBalance";
        }
        SendControlJson(json, coalesceKey);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        EyeCamStudioWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<EyeCamStudioWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<EyeCamStudioWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

        switch (msg) {
            case WM_ERASEBKGND:
                return 1;

            case WM_SIZE:
                if (self->m_pRenderTarget) {
                    UINT32 w = (std::max)(1u, static_cast<UINT32>(LOWORD(lParam)));
                    UINT32 h = (std::max)(1u, static_cast<UINT32>(HIWORD(lParam)));
                    self->ReleaseOffscreenBlurTargets();
                    self->m_pRenderTarget->Resize(D2D1::SizeU(w, h));
                }
                return 0;

            case WM_TIMER:
                if (wParam == 2 && self->m_state.transport == TransportTab::USB_ADB) {
                    self->RefreshUsbDevicesAsync();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;

            case WM_PAINT: {
                PAINTSTRUCT ps{};
                BeginPaint(hwnd, &ps);
                self->OnPaint();
                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_LBUTTONDOWN:
                SetCapture(hwnd);
                self->HandleMouseDown(
                    static_cast<float>(GET_X_LPARAM(lParam)),
                    static_cast<float>(GET_Y_LPARAM(lParam))
                );
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;

            case WM_MOUSEMOVE: {
                int dragId = self->m_activeSliderDrag.load();
                if (dragId == 100 && (wParam & MK_LBUTTON)) {
                    self->UpdateZoomSliderValue(static_cast<float>(GET_X_LPARAM(lParam)));
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (dragId >= 0 && (wParam & MK_LBUTTON)) {
                    self->UpdateSliderValue(
                        dragId,
                        static_cast<float>(GET_X_LPARAM(lParam))
                    );
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            case WM_LBUTTONUP:
                ReleaseCapture();
                self->m_activeSliderDrag = -1;
                return 0;

            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE && self->m_state.activeModal != ModalOverlay::NONE) {
                    self->m_state.activeModal = ModalOverlay::NONE;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                break;

            case WM_CHAR:
                if (self->m_state.ipInputFocused) {
                    wchar_t ch = static_cast<wchar_t>(wParam);
                    if (ch == L'\b') {
                        if (!self->m_state.deviceIp.empty()) self->m_state.deviceIp.pop_back();
                    } else if ((ch >= L'0' && ch <= L'9') || ch == L'.') {
                        if (self->m_state.deviceIp.size() < 15) {
                            self->m_state.deviceIp.push_back(static_cast<char>(ch));
                        }
                    }
                    CloseSocketAndReconnect(self);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;

            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    static void CloseSocketAndReconnect(EyeCamStudioWindow* self) {
        self->CloseActivePhoneSocket();
    }

private:
    HWND m_hwnd = nullptr;
    ID2D1Factory* m_pD2DFactory = nullptr;
    IDWriteFactory* m_pDWriteFactory = nullptr;
    ID2D1HwndRenderTarget* m_pRenderTarget = nullptr;
    ID2D1RenderTarget* m_activeRT = nullptr;
    ID2D1BitmapRenderTarget* m_pSceneRT = nullptr;
    ID2D1BitmapRenderTarget* m_pBlurSmallRT = nullptr;
    ID2D1BitmapRenderTarget* m_pBlurPingRT = nullptr;
    UINT32 m_blurTargetW = 0;
    UINT32 m_blurTargetH = 0;
    ID2D1SolidColorBrush* m_pBrush = nullptr;
    ID2D1Bitmap* m_pPreviewBitmap = nullptr;

    IDWriteTextFormat* m_pFontTitle = nullptr;
    IDWriteTextFormat* m_pFontSection = nullptr;
    IDWriteTextFormat* m_pFontBody = nullptr;
    IDWriteTextFormat* m_pFontMono = nullptr;

    StudioState m_state{};
    CubeLut3D m_cubeLut{};
    EyeCam::WindowsDirectShowSink m_sink{};

    std::atomic<bool> m_running{false};
    bool m_mfStarted = false;
    std::atomic<bool> m_usbScanInProgress{false};
    std::atomic<long long> m_lastKeyframeReqMs{0};
    std::mutex m_socketMutex;
    std::mutex m_usbMutex;
    std::mutex m_frameMutex;
    std::mutex m_sinkMutex;
    std::mutex m_decodeMutex;
    std::condition_variable m_decodeCv;
    std::deque<IncomingVideoPacket> m_decodeQueue;
    std::mutex m_sendMutex;
    std::condition_variable m_sendCv;
    std::deque<OutboundControlPacket> m_sendQueue;
    SOCKET m_phoneSocket = INVALID_SOCKET;
    std::thread m_streamThread;
    std::thread m_decodeThread;
    std::thread m_sendThread;

    uint32_t m_frameTick = 0;
    bool m_hasLiveFrame = false;
    std::vector<uint8_t> m_previewPixels;
    std::vector<uint8_t> m_yuy2Buffer;
    std::vector<uint8_t> m_fullBgraBuffer;
    std::vector<uint8_t> m_cachedPreviewBuffer;

    // Інтерактивні зони UI
    UiRect m_rcBtnHeaderLang{};
    UiRect m_rcBtnHeaderSupport{};
    UiRect m_rcBtnHeaderGithub{};
    UiRect m_rcTransportTabs[2]{};
    UiRect m_rcIpInput{};
    UiRect m_rcUsbDropdown{};
    UiRect m_rcBtnRefreshUsb{};
    std::vector<UiRect> m_rcUsbItems;
    UiRect m_rcBtnStartStop{};
    UiRect m_rcBtnRegisterDriver{};
    UiRect m_rcVideoViewport{};
    UiRect m_rcModuleBtns[5]{};
    UiRect m_rcZoomSlider{};
    UiRect m_rcResBtns[4]{};
    UiRect m_rcBtnModeToggle{};
    UiRect m_rcBtnRefocus{};
    UiRect m_rcBtnAeLock{};
    UiRect m_rcBtnTorch{};
    UiRect m_rcSliders[4]{};
    UiRect m_rcSliderLocks[4]{};
    UiRect m_rcBtnLoadLut{};
    UiRect m_rcBtnClearLut{};
    UiRect m_rcBtnMirror{};
    UiRect m_rcBtnGrid{};
    UiRect m_rcBtnRotate{};
    UiRect m_rcModalCard{};
    UiRect m_rcModalClose{};
    UiRect m_rcModalPrimaryBtn{};
    UiRect m_rcModalSecondaryBtn{};
    std::atomic<int> m_activeSliderDrag{-1};
};

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)pCmdLine;
    SetProcessDPIAware();

    EyeCamStudioWindow app;
    if (!app.Initialize(hInstance, nCmdShow)) {
        return 1;
    }
    return app.RunMessageLoop();
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    return wWinMain(hInstance, nullptr, GetCommandLineW(), nCmdShow);
}

int main() {
    return wWinMain(GetModuleHandleW(nullptr), nullptr, GetCommandLineW(), SW_SHOWDEFAULT);
}

#endif // _WIN32
