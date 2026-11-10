#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <ole2.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using std::min;
using std::max;
#include <gdiplus.h>

#include "WindowsDirectShowSink.hpp"
#include "SharedMemoryProtocol.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

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
const D2D1_COLOR_F COL_BG_DROPDOWN   = HexColor(0x0F141D);
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
        return D2D1::RectF(left, top, right, bottom);
    }
    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
};

enum class TransportTab {
    WIFI_LAN = 0,
    USB_ADB  = 1
};

struct UsbDeviceEntry {
    std::string serial;
    std::wstring displayName;
    bool authorized = true;
};

struct StudioState {
    // Підключення та транспорт (лише Wi-Fi LAN та USB ADB — без зайвого тест-потоку)
    TransportTab transport = TransportTab::USB_ADB;
    std::string deviceIp = "192.168.1.105";
    bool ipInputFocused = false;
    bool isStreaming = true;
    bool isDriverRegistered = false;
    std::atomic<bool> isPhoneConnected{false};

    // Випадаючий список USB пристроїв (ADB)
    std::vector<UsbDeviceEntry> usbDevices;
    int selectedUsbIdx = 0;
    bool usbDropdownOpen = false;

    // Профіль підключеного смартфона (наприклад, Samsung Galaxy S26 Ultra)
    std::wstring connectedPhoneName = L"Очікування підключення смартфона...";
    float phoneMinZoom = 0.5f;
    float phoneMaxZoom = 10.0f;
    bool phoneSupports4K = true;

    // Параметри відеопотоку (включаючи 4K UHD та 2K QHD)
    int resPresetIdx = 2; // 0: 4K UHD (3840x2160), 1: 2K QHD (2560x1440), 2: 1080p 60, 3: 720p 60
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;

    // Керування оптикою та зумом (0.5x, 1x, 2x, 3x, 5x, 10x, Front)
    int activeLensIdx = 1; // 0: 0.5x, 1: 1x, 2: 2x, 3: 3x, 4: 5x, 5: 10x, 6: Front
    float currentZoom = 1.0f;
    float zoomSliderNorm = 0.05f; // Плавний зум від phoneMinZoom до phoneMaxZoom
    bool isProManual = false;
    bool mirrorHorizontal = false;
    bool showGrid = false;

    // Повзунки Pro режиму (0.0 .. 1.0 нормалізовані)
    float isoNorm = 0.20f;     // 100 .. 3200
    float shutterNorm = 0.35f; // 1/30 .. 1/4000
    float focusNorm = 0.0f;    // 0.0 .. 1.0
    float wbNorm = 0.48f;      // 2800K .. 8500K

    // Телеметрія реального часу
    std::atomic<uint64_t> rxFrames{0};
    std::atomic<uint64_t> rxBytes{0};
    std::atomic<uint32_t> measuredBitrateKbps{0};
    std::atomic<uint32_t> measuredFps{0};
    uint64_t shmFramesWritten = 0;
};

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (sz <= 0) return L"";
    std::wstring res(sz, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), res.data(), sz);
    return res;
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

    // 1. Вбудований у директорію інсталятора {app}\adb\adb.exe
    std::wstring bundled1 = dirPath + L"\\adb\\adb.exe";
    if (GetFileAttributesW(bundled1.c_str()) != INVALID_FILE_ATTRIBUTES) return bundled1;

    // 2. Поруч із EyeCamStudio.exe
    std::wstring bundled2 = dirPath + L"\\adb.exe";
    if (GetFileAttributesW(bundled2.c_str()) != INVALID_FILE_ATTRIBUTES) return bundled2;

    // 3. Стандартний шлях Android SDK у профілі користувача
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
std::vector<UsbDeviceEntry> ScanConnectedUsbDevices() {
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

        // Розбір рядка: "<serial>   device product:... model:SM_S948B device:..."
        size_t spacePos = line.find_first_of(" \t");
        if (spacePos == std::string::npos) continue;
        std::string serial = line.substr(0, spacePos);
        std::string rest = line.substr(spacePos);

        // Ігноруємо мережеві ADB-з'єднання, залишаємо фізичні USB
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

    // Якщо є авторизований пристрій — автоматично прокидаємо порт TCP 8554 для нього
    for (const auto& dev : list) {
        if (dev.authorized) {
            std::wstring wSer = Utf8ToWide(dev.serial);
            RunHiddenCommandOutput(L"\"" + adbPath + L"\" -s " + wSer + L" forward tcp:8554 tcp:8554");
            break;
        }
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

// Простий витягувач числових та рядкових полів із JSON пакета можливостей смартфона
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
    outVal = static_cast<float>(std::atof(json.c_str() + start));
    return true;
}

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

        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, nullptr);

        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pD2DFactory);
        if (FAILED(hr)) return false;

        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&m_pDWriteFactory)
        );
        if (FAILED(hr)) return false;

        CreateTextFormats();

        // Завантаження іконки ноутбука з оком та бліком об'єктива (ресурс ID 101)
        HICON hAppIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
        if (!hAppIcon) hAppIcon = LoadIconW(nullptr, IDI_APPLICATION);

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
            L"EyeCam Studio — Ultra-Low Latency 4K Virtual Webcam",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 780,
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

        // Початкове сканування USB ADB пристроїв у фоновому потоці
        RefreshUsbDevicesAsync();

        // Запуск каналу спільної пам'яті DirectShow віртуальної камери
        m_sink.Open(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2, m_state.fps);

        // Запуск фонового потоку підключення до смартфона (TCP :8554 — відео + керування)
        m_running = true;
        m_streamThread = std::thread(&EyeCamStudioWindow::PhoneStreamClientLoop, this);

        SetTimer(m_hwnd, 1, 30, nullptr);   // ~33 FPS оновлення UI
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

    void Shutdown() {
        m_running = false;
        CloseActivePhoneSocket();
        if (m_streamThread.joinable()) {
            m_streamThread.join();
        }
        m_sink.Close();

        if (m_pPreviewBitmap) { m_pPreviewBitmap->Release(); m_pPreviewBitmap = nullptr; }
        if (m_pBrush) { m_pBrush->Release(); m_pBrush = nullptr; }
        if (m_pRenderTarget) { m_pRenderTarget->Release(); m_pRenderTarget = nullptr; }
        if (m_pFontTitle) { m_pFontTitle->Release(); m_pFontTitle = nullptr; }
        if (m_pFontSection) { m_pFontSection->Release(); m_pFontSection = nullptr; }
        if (m_pFontBody) { m_pFontBody->Release(); m_pFontBody = nullptr; }
        if (m_pFontMono) { m_pFontMono->Release(); m_pFontMono = nullptr; }
        if (m_pDWriteFactory) { m_pDWriteFactory->Release(); m_pDWriteFactory = nullptr; }
        if (m_pD2DFactory) { m_pD2DFactory->Release(); m_pD2DFactory = nullptr; }
        if (m_gdiplusToken) {
            Gdiplus::GdiplusShutdown(m_gdiplusToken);
            m_gdiplusToken = 0;
        }
        WSACleanup();
    }

    void CloseActivePhoneSocket() {
        std::lock_guard<std::mutex> lock(m_socketMutex);
        if (m_phoneSocket != INVALID_SOCKET) {
            closesocket(m_phoneSocket);
            m_phoneSocket = INVALID_SOCKET;
        }
    }

    void RefreshUsbDevicesAsync() {
        if (m_usbScanInProgress.exchange(true)) return;
        std::thread([this]() {
            auto found = ScanConnectedUsbDevices();
            {
                std::lock_guard<std::mutex> lock(m_usbMutex);
                m_state.usbDevices = std::move(found);
                if (m_state.selectedUsbIdx >= static_cast<int>(m_state.usbDevices.size())) {
                    m_state.selectedUsbIdx = 0;
                }
            }
            m_usbScanInProgress = false;
            if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
        }).detach();
    }

    // Надсилання JSON-команди керування смартфону по активному або новому сокету
    void SendControlJson(const std::string& jsonPayload) {
        std::thread([this, jsonPayload]() {
            uint8_t header[8];
            header[0] = 0x43; // 'C'
            header[1] = 0x50; // 'P'
            header[2] = 0x00;
            header[3] = 0x01; // MSG_TYPE_JSON_COMMAND = 1
            uint32_t len = static_cast<uint32_t>(jsonPayload.size());
            header[4] = static_cast<uint8_t>((len >> 24) & 0xFF);
            header[5] = static_cast<uint8_t>((len >> 16) & 0xFF);
            header[6] = static_cast<uint8_t>((len >> 8) & 0xFF);
            header[7] = static_cast<uint8_t>(len & 0xFF);

            std::lock_guard<std::mutex> lock(m_socketMutex);
            if (m_phoneSocket != INVALID_SOCKET) {
                send(m_phoneSocket, reinterpret_cast<const char*>(header), 8, 0);
                send(m_phoneSocket, jsonPayload.data(), static_cast<int>(jsonPayload.size()), 0);
            }
        }).detach();
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

    // Постійний клієнтський потік прийому живого відео та метаданих зі смартфона (USB ADB або Wi-Fi LAN)
    void PhoneStreamClientLoop() {
        auto lastStatTime = std::chrono::steady_clock::now();
        uint64_t bytesWindow = 0;
        uint32_t framesWindow = 0;

        while (m_running) {
            if (!m_state.isStreaming) {
                m_state.isPhoneConnected = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            std::string targetIp = (m_state.transport == TransportTab::USB_ADB) ? "127.0.0.1" : m_state.deviceIp;

            SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            // Неблокуючий connect з таймаутом 600 мс
            u_long nonBlocking = 1;
            ioctlsocket(sock, FIONBIO, &nonBlocking);

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(8554);
            if (inet_pton(AF_INET, targetIp.c_str(), &addr.sin_addr) <= 0) {
                closesocket(sock);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                continue;
            }

            nonBlocking = 0;
            ioctlsocket(sock, FIONBIO, &nonBlocking);
            DWORD rcvTimeout = 2500;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcvTimeout), sizeof(rcvTimeout));

            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                m_phoneSocket = sock;
            }
            m_state.isPhoneConnected = true;

            // Запитуємо можливості камери підключеного телефону
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

                if (msgType == 7 && !payload.empty()) {
                    // MSG_TYPE_VIDEO_FRAME = 7 (Живий JPEG-кадр із камери смартфона)
                    DecodeIncomingJpegFrame(payload.data(), payload.size());
                    m_state.rxFrames++;
                    framesWindow++;
                } else if (msgType == 1 && !payload.empty()) {
                    // MSG_TYPE_JSON_COMMAND = 1 (Профіль можливостей телефону або телеметрія)
                    std::string jsonStr(reinterpret_cast<const char*>(payload.data()), payload.size());
                    ParseDeviceCapabilitiesJson(jsonStr);
                }

                auto now = std::chrono::steady_clock::now();
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatTime).count();
                if (elapsedMs >= 1000) {
                    m_state.measuredBitrateKbps = static_cast<uint32_t>((bytesWindow * 8) / static_cast<uint64_t>(elapsedMs));
                    m_state.measuredFps = static_cast<uint32_t>((static_cast<uint64_t>(framesWindow) * 1000) / static_cast<uint64_t>(elapsedMs));
                    bytesWindow = 0;
                    framesWindow = 0;
                    lastStatTime = now;
                }
            }

            CloseActivePhoneSocket();
            m_state.isPhoneConnected = false;
            m_state.measuredBitrateKbps = 0;
            m_state.measuredFps = 0;
        }
    }

    void ParseDeviceCapabilitiesJson(const std::string& json) {
        std::string devName;
        if (ExtractJsonString(json, "device_name", devName) && !devName.empty()) {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            m_state.connectedPhoneName = Utf8ToWide(devName);
        }
        float minZ = 0.0f;
        if (ExtractJsonFloat(json, "min_zoom", minZ) && minZ > 0.1f) {
            m_state.phoneMinZoom = minZ;
        }
        float maxZ = 0.0f;
        if (ExtractJsonFloat(json, "max_zoom", maxZ) && maxZ >= 1.0f) {
            m_state.phoneMaxZoom = maxZ;
        }
    }

    // Декодування живого JPEG-кадру зі смартфона у BGRA прев'ю та YUY2 буфер DirectShow віртуальної камери
    void DecodeIncomingJpegFrame(const uint8_t* jpegData, size_t jpegLen) {
        IStream* pStream = SHCreateMemStream(jpegData, static_cast<UINT>(jpegLen));
        if (!pStream) return;

        Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(pStream);
        if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
            const uint32_t pw = 640;
            const uint32_t ph = 360;

            Gdiplus::Bitmap scaled(static_cast<INT>(pw), static_cast<INT>(ph), PixelFormat32bppARGB);
            {
                Gdiplus::Graphics g(&scaled);
                g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
                g.DrawImage(bmp, 0, 0, static_cast<INT>(pw), static_cast<INT>(ph));
            }

            Gdiplus::Rect lockRect(0, 0, static_cast<INT>(pw), static_cast<INT>(ph));
            Gdiplus::BitmapData bmpData{};
            if (scaled.LockBits(&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmpData) == Gdiplus::Ok) {
                std::vector<uint8_t> localPreview(pw * ph * 4);
                for (uint32_t y = 0; y < ph; ++y) {
                    const uint8_t* srcLine = static_cast<const uint8_t*>(bmpData.Scan0) +
                                             static_cast<intptr_t>(y) * static_cast<intptr_t>(bmpData.Stride);
                    uint8_t* dstLine = localPreview.data() + y * pw * 4;
                    if (!m_state.mirrorHorizontal) {
                        std::memcpy(dstLine, srcLine, pw * 4);
                    } else {
                        for (uint32_t x = 0; x < pw; ++x) {
                            uint32_t sx = pw - 1 - x;
                            dstLine[x * 4 + 0] = srcLine[sx * 4 + 0];
                            dstLine[x * 4 + 1] = srcLine[sx * 4 + 1];
                            dstLine[x * 4 + 2] = srcLine[sx * 4 + 2];
                            dstLine[x * 4 + 3] = 255;
                        }
                    }
                }
                scaled.UnlockBits(&bmpData);

                {
                    std::lock_guard<std::mutex> lock(m_frameMutex);
                    m_previewPixels = std::move(localPreview);
                    m_hasLiveFrame = true;
                }

                // Запис кадру у спільну пам'ять віртуальної камери WindowsDirectShowSink (до 4K UHD)
                PushFrameToDirectShow();
            }
        }
        delete bmp;
        pStream->Release();
    }

    void PushFrameToDirectShow() {
        const uint32_t pw = 640;
        const uint32_t ph = 360;
        std::vector<uint8_t> copyPixels;
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            if (m_previewPixels.size() != pw * ph * 4) return;
            copyPixels = m_previewPixels;
        }

        const uint32_t outW = m_state.width;
        const uint32_t outH = m_state.height;
        size_t expectedYuy2 = EyeCam::CalculateFrameSize(outW, outH, EyeCam::PixelFormat::YUY2);
        if (m_yuy2Buffer.size() != expectedYuy2) {
            m_yuy2Buffer.resize(expectedYuy2);
        }

        for (uint32_t y = 0; y < outH; ++y) {
            uint32_t srcY = (y * ph) / outH;
            const uint8_t* srcRow = copyPixels.data() + (srcY * pw * 4);
            uint8_t* dstRow = m_yuy2Buffer.data() + (y * outW * 2);

            for (uint32_t x = 0; x < outW; x += 2) {
                uint32_t srcX = (x * pw) / outW;
                const uint8_t* p = srcRow + (srcX * 4);
                uint8_t b = p[0];
                uint8_t g = p[1];
                uint8_t r = p[2];

                uint8_t yVal = static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                uint8_t uVal = static_cast<uint8_t>(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                uint8_t vVal = static_cast<uint8_t>(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);

                size_t dIdx = x * 2;
                dstRow[dIdx + 0] = yVal;
                dstRow[dIdx + 1] = uVal;
                dstRow[dIdx + 2] = yVal;
                dstRow[dIdx + 3] = vVal;
            }
        }

        if (m_sink.IsOpen()) {
            if (m_sink.WriteFrame(m_yuy2Buffer.data(), m_yuy2Buffer.size())) {
                m_state.shmFramesWritten++;
            }
        }
    }

    void EnsureRenderTarget() {
        if (!m_pRenderTarget) {
            RECT rc{};
            GetClientRect(m_hwnd, &rc);
            D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

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
                m_pRenderTarget->CreateBitmap(D2D1::SizeU(640, 360), bmpProps, &m_pPreviewBitmap);
            }
        }
    }

    void DrawRoundedCard(const UiRect& r, const D2D1_COLOR_F& fill, const D2D1_COLOR_F& border, float radius = 10.0f) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r.ToD2D(), radius, radius);
        m_pBrush->SetColor(fill);
        m_pRenderTarget->FillRoundedRectangle(rr, m_pBrush);
        m_pBrush->SetColor(border);
        m_pRenderTarget->DrawRoundedRectangle(rr, m_pBrush, 1.0f);
    }

    void DrawTextStr(const std::wstring& text, const UiRect& r, IDWriteTextFormat* fmt, const D2D1_COLOR_F& color,
                     DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!fmt) return;
        fmt->SetTextAlignment(align);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_pBrush->SetColor(color);
        m_pRenderTarget->DrawText(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            fmt,
            r.ToD2D(),
            m_pBrush
        );
    }

    // Малювання фірмової іконки ПК-застосунку (Рамка ноутбука + Око з бліком об'єктива) у шапці вікна
    void DrawLaptopEyeLogo(float x, float y, float h) {
        float cx = x + h * 0.55f;
        float cy = y + h * 0.46f;

        // 1. Рамка екрана ноутбука
        UiRect screenRect{x + 2.0f, y + 4.0f, x + h * 1.10f - 2.0f, y + h - 8.0f};
        DrawRoundedCard(screenRect, HexColor(0x0D131D), COL_CYAN, 4.0f);

        // Нижня дека ноутбука
        UiRect baseRect{x - 2.0f, y + h - 7.0f, x + h * 1.10f + 2.0f, y + h - 3.0f};
        DrawRoundedCard(baseRect, COL_BG_CARD, COL_CYAN, 2.0f);

        // 2. Зовнішній контур ока (Смарагдовий)
        m_pBrush->SetColor(COL_EMERALD);
        m_pRenderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 12.5f, 7.5f), m_pBrush, 1.8f);

        // 3. Лінза об'єктива всередині ока
        m_pBrush->SetColor(HexColor(0x0B2536));
        m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.2f, 6.2f), m_pBrush);
        m_pBrush->SetColor(COL_CYAN);
        m_pRenderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.2f, 6.2f), m_pBrush, 1.4f);

        // Зіниця
        m_pBrush->SetColor(HexColor(0x04070B));
        m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.0f, 3.0f), m_pBrush);

        // 4. Блік як від об'єктиву (верхній лівий кут лінзи)
        m_pBrush->SetColor(HexColor(0xFFFFFF, 0.95f));
        m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 2.2f, cy - 2.2f), 2.0f, 1.4f), m_pBrush);
    }

    void OnPaint() {
        EnsureRenderTarget();
        if (!m_pRenderTarget) return;

        m_frameTick++;
        m_pRenderTarget->BeginDraw();
        m_pRenderTarget->Clear(COL_BG_MAIN);

        D2D1_SIZE_F sz = m_pRenderTarget->GetSize();
        float W = sz.width;
        float H = sz.height;

        const float pad = 16.0f;
        const float leftW = 305.0f;
        const float rightW = 325.0f;
        const float topBarH = 56.0f;

        // ====================================================================
        // 1. ВЕРХНІЙ ЗАГОЛОВОК СТУДІЇ (Header Bar з іконкою Ноутбук+Око)
        // ====================================================================
        UiRect headerRect{pad, pad, W - pad, pad + topBarH};
        DrawRoundedCard(headerRect, COL_BG_PANEL, COL_BORDER, 12.0f);

        DrawLaptopEyeLogo(pad + 16.0f, pad + 10.0f, 36.0f);
        DrawTextStr(L"EYECAM STUDIO", {pad + 64, pad, pad + 290, pad + topBarH}, m_pFontTitle, COL_TEXT_PRIMARY);
        DrawTextStr(L"4K UHD DirectShow & ADB/Wi-Fi Pro Camera Engine", {pad + 245, pad + 2, pad + 680, pad + topBarH}, m_pFontBody, COL_TEXT_SECOND);

        // Індикатор статусу драйвера DirectShow у шапці
        UiRect drvBadge{W - pad - 250, pad + 12, W - pad - 16, pad + topBarH - 12};
        DrawRoundedCard(drvBadge, m_state.isDriverRegistered ? HexColor(0x092E20) : HexColor(0x332409),
                        m_state.isDriverRegistered ? COL_EMERALD : COL_AMBER, 16.0f);
        DrawTextStr(
            m_state.isDriverRegistered ? L"● DirectShow Webcam: ACTIVE" : L"▲ DirectShow: НЕ ЗАРЕЄСТРОВАНО",
            drvBadge, m_pFontMono, m_state.isDriverRegistered ? COL_EMERALD : COL_AMBER,
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

        DrawTextStr(L"1. СПОСІБ ПІДКЛЮЧЕННЯ", {lx, cy, rx, cy + 24}, m_pFontSection, COL_TEXT_PRIMARY);
        cy += 30;

        // Лише 2 вкладки: Wi-Fi (LAN) та USB (Кабель) — без тест-потоку
        float tabW = (rx - lx - 8.0f) * 0.5f;
        const wchar_t* tabNames[2] = {L"Wi-Fi (LAN)", L"USB (Кабель)"};
        for (int i = 0; i < 2; ++i) {
            m_rcTransportTabs[i] = {lx + i * (tabW + 8.0f), cy, lx + i * (tabW + 8.0f) + tabW, cy + 36.0f};
            bool active = (static_cast<int>(m_state.transport) == i);
            DrawRoundedCard(m_rcTransportTabs[i], active ? COL_CYAN : COL_BG_CARD, active ? COL_CYAN : COL_BORDER, 8.0f);
            DrawTextStr(tabNames[i], m_rcTransportTabs[i], m_pFontSection,
                        active ? HexColor(0x05080D) : COL_TEXT_PRIMARY, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        cy += 48;

        if (m_state.transport == TransportTab::WIFI_LAN) {
            // У режимі Wi-Fi показуємо поле вводу IP-адреси смартфона
            DrawTextStr(L"IP-адреса смартфона (див. у додатку):", {lx, cy, rx, cy + 20}, m_pFontBody, COL_TEXT_SECOND);
            cy += 24;
            m_rcIpInput = {lx, cy, rx, cy + 38};
            m_rcUsbDropdown = {0, 0, 0, 0};
            m_rcBtnRefreshUsb = {0, 0, 0, 0};

            DrawRoundedCard(m_rcIpInput, HexColor(0x0D1117), m_state.ipInputFocused ? COL_CYAN : COL_BORDER, 8.0f);
            std::wstring ipW(m_state.deviceIp.begin(), m_state.deviceIp.end());
            if (m_state.ipInputFocused && ((m_frameTick / 15) % 2 == 0)) ipW += L"|";
            DrawTextStr(L"  " + ipW, m_rcIpInput, m_pFontMono, COL_TEXT_PRIMARY);
            cy += 46;

            DrawTextStr(L"Порт синхронізації та відео (TCP): 8554", {lx, cy, rx, cy + 20}, m_pFontBody, COL_TEXT_DIM);
            cy += 30;
        } else {
            // У режимі USB жодної IP-адреси не питаємо — показуємо випадаючий список USB пристроїв!
            m_rcIpInput = {0, 0, 0, 0};
            DrawTextStr(L"Виберіть смартфон, підключений по USB:", {lx, cy, rx, cy + 20}, m_pFontBody, COL_TEXT_SECOND);
            cy += 24;

            m_rcUsbDropdown = {lx, cy, rx, cy + 38};
            DrawRoundedCard(m_rcUsbDropdown, HexColor(0x0D1117), m_state.usbDropdownOpen ? COL_CYAN : COL_BORDER, 8.0f);

            std::wstring selectedUsbLabel = L"  Пристроїв USB не виявлено";
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

            // Кнопка оновлення списку USB-пристроїв
            m_rcBtnRefreshUsb = {lx, cy, rx, cy + 32};
            DrawRoundedCard(m_rcBtnRefreshUsb, COL_BG_CARD, COL_BORDER, 8.0f);
            DrawTextStr(L"↻  Оновити список USB-пристроїв", m_rcBtnRefreshUsb, m_pFontBody, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
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
            m_state.isStreaming ? L"■  ЗУПИНИТИ ВІРТУАЛЬНУ КАМЕРУ" : L"▶  ЗАПУСТИТИ ТРАНСЛЯЦІЮ",
            m_rcBtnStartStop, m_pFontSection,
            m_state.isStreaming ? COL_CRIMSON : HexColor(0x06120C),
            DWRITE_TEXT_ALIGNMENT_CENTER
        );
        cy += 58;

        // Блок реєстрації системного драйвера DirectShow
        DrawTextStr(L"2. СИСТЕМНИЙ ДРАЙВЕР КАМЕРИ", {lx, cy, rx, cy + 24}, m_pFontSection, COL_TEXT_PRIMARY);
        cy += 28;

        UiRect drvCard{lx, cy, rx, cy + 126};
        DrawRoundedCard(drvCard, COL_BG_CARD, COL_BORDER, 10.0f);
        DrawTextStr(L"Камера: EyeCam Virtual Camera", {lx + 12, cy + 8, rx - 12, cy + 30}, m_pFontBody, COL_TEXT_PRIMARY);
        DrawTextStr(
            m_state.isDriverRegistered
                ? L"Статус: Зареєстровано в системі\nДоступна в OBS, Zoom, Discord, Teams"
                : L"Статус: Потрібна реєстрація DLL\nНатисніть кнопку нижче (права Admin)",
            {lx + 12, cy + 32, rx - 12, cy + 76}, m_pFontBody,
            m_state.isDriverRegistered ? COL_EMERALD : COL_AMBER
        );

        m_rcBtnRegisterDriver = {lx + 12, cy + 82, rx - 12, cy + 116};
        DrawRoundedCard(m_rcBtnRegisterDriver, COL_BG_PANEL, COL_CYAN, 8.0f);
        DrawTextStr(
            m_state.isDriverRegistered ? L"↻ Перереєструвати драйвер (UAC)" : L"⚡ Зареєструвати камеру в системі",
            m_rcBtnRegisterDriver, m_pFontBody, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER
        );

        // Якщо відкритий випадаючий список USB — малюємо його поверх лівої панелі
        if (m_state.transport == TransportTab::USB_ADB && m_state.usbDropdownOpen) {
            std::lock_guard<std::mutex> lock(m_usbMutex);
            int count = static_cast<int>(m_state.usbDevices.size());
            int rows = (count > 0) ? count : 1;
            UiRect dropBox{m_rcUsbDropdown.left, m_rcUsbDropdown.bottom + 2, m_rcUsbDropdown.right, m_rcUsbDropdown.bottom + 2 + rows * 34.0f};
            DrawRoundedCard(dropBox, COL_BG_DROPDOWN, COL_CYAN, 8.0f);

            m_rcUsbItems.clear();
            if (count == 0) {
                DrawTextStr(L"  Немає активних USB ADB пристроїв", dropBox, m_pFontBody, COL_TEXT_SECOND);
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
        // 3. ЦЕНТРАЛЬНИЙ В'ЮПОРТ (16:9 Live Video + Телеметрія)
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
        UiRect videoRect{vx, vy, vx + videoW, vy + videoH};

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
            m_pRenderTarget->DrawBitmap(m_pPreviewBitmap, videoRect.ToD2D(), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            DrawRoundedCard(videoRect, HexColor(0x070A0F), COL_BORDER, 8.0f);

            // Інформативний екран очікування підключення смартфона (без штучного тест-генератора)
            float midY = (videoRect.top + videoRect.bottom) * 0.5f;
            DrawLaptopEyeLogo((videoRect.left + videoRect.right) * 0.5f - 22.0f, midY - 68.0f, 40.0f);

            if (!m_state.isStreaming) {
                DrawTextStr(L"ТРАНСЛЯЦІЮ ПРИЗУПИНЕНО", {videoRect.left, midY - 12, videoRect.right, midY + 18},
                            m_pFontTitle, COL_TEXT_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (m_state.transport == TransportTab::USB_ADB) {
                DrawTextStr(L"ОЧІКУВАННЯ ВІДЕОПОТОКУ ПО USB (ADB)...", {videoRect.left, midY - 14, videoRect.right, midY + 14},
                            m_pFontSection, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
                DrawTextStr(
                    L"1. Підключіть телефон кабелем USB та увімкніть «Налагодження по USB»\n"
                    L"2. Відкрийте EyeCam на смартфоні — трансляція запуститься автоматично",
                    {videoRect.left + 20, midY + 20, videoRect.right - 20, midY + 72},
                    m_pFontBody, COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER
                );
            } else {
                std::wstring waitWifi = L"ОЧІКУВАННЯ ПІДКЛЮЧЕННЯ ДО " + Utf8ToWide(m_state.deviceIp) + L":8554...";
                DrawTextStr(waitWifi, {videoRect.left, midY - 14, videoRect.right, midY + 14},
                            m_pFontSection, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
                DrawTextStr(
                    L"Перевірте IP-адресу у верхньому лівому куті додатку EyeCam на телефоні\n"
                    L"та натисніть зелену кнопку Play у мобільному застосунку.",
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
                m_pRenderTarget->DrawLine(D2D1::Point2F(gx, videoRect.top), D2D1::Point2F(gx, videoRect.bottom), m_pBrush, 1.0f);
                m_pRenderTarget->DrawLine(D2D1::Point2F(videoRect.left, gy), D2D1::Point2F(videoRect.right, gy), m_pBrush, 1.0f);
            }
        }

        // Рамка в'юпорту
        m_pBrush->SetColor(m_state.isPhoneConnected ? COL_EMERALD : COL_BORDER);
        m_pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(videoRect.ToD2D(), 6.0f, 6.0f), m_pBrush, 1.5f);

        // Плаваючий бейдж формату та статусу поверх відео
        wchar_t badgeBuf[160]{};
        swprintf_s(
            badgeBuf,
            L" %s  •  %ux%u @ %u FPS  •  ЗУМ %.1fx ",
            m_state.isPhoneConnected ? L"● ONLINE" : L"○ STANDBY",
            m_state.width, m_state.height, m_state.fps, m_state.currentZoom
        );
        UiRect hudBadge1{videoRect.left + 12, videoRect.top + 12, videoRect.left + 325, videoRect.top + 38};
        DrawRoundedCard(hudBadge1, HexColor(0x0B0E14, 0.86f), m_state.isPhoneConnected ? COL_EMERALD : COL_BORDER, 6.0f);
        DrawTextStr(badgeBuf, hudBadge1, m_pFontMono, m_state.isPhoneConnected ? COL_EMERALD : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);

        // Нижній телеметричний бар під відео
        float barY = centerPanel.bottom - 52.0f;
        UiRect teleBar{centerPanel.left + 16, barY, centerPanel.right - 16, centerPanel.bottom - 14};
        DrawRoundedCard(teleBar, COL_BG_CARD, COL_BORDER, 8.0f);

        wchar_t teleStr[256]{};
        swprintf_s(
            teleStr,
            L"  Пристрій: %s   |   Бітрейт: %.1f Мбіт/с   |   Кадрів: %llu",
            m_state.connectedPhoneName.c_str(),
            m_state.measuredBitrateKbps.load() / 1000.0f,
            static_cast<unsigned long long>(m_state.rxFrames.load())
        );
        DrawTextStr(teleStr, teleBar, m_pFontMono, COL_TEXT_SECOND);

        // ====================================================================
        // 4. ПРАВА ПАНЕЛЬ: Оптика та Зум до 10x, 4K UHD, Повзунки ISP
        // ====================================================================
        UiRect rightPanel{W - pad - rightW, contentTop, W - pad, contentBottom};
        DrawRoundedCard(rightPanel, COL_BG_PANEL, COL_BORDER, 12.0f);

        float rlx = rightPanel.left + 16;
        float rrx = rightPanel.right - 16;
        float rcy = rightPanel.top + 16;

        wchar_t lensTitle[128]{};
        swprintf_s(lensTitle, L"3. ОПТИКА ТА ЗУМ (МАКС: %.0fx)", m_state.phoneMaxZoom);
        DrawTextStr(lensTitle, {rlx, rcy, rrx, rcy + 22}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 26;

        // 7 кнопок пресетів зуму та об'єктивів: 0.5x, 1x, 2x, 3x, 5x, 10x, Front
        // Кнопки автоматично адаптуються до того, чи підтримує телефон відповідний зум (напр. S26 Ultra -> до 10x)
        const wchar_t* lensLabels[7] = {L"0.5x", L"1x", L"2x", L"3x", L"5x", L"10x", L"Front"};
        const float lensZooms[7]     = {0.5f,    1.0f,  2.0f,  3.0f,  5.0f,  10.0f,  1.0f};
        float btnGap = 5.0f;
        float lensW = (rrx - rlx - btnGap * 6.0f) / 7.0f;

        for (int i = 0; i < 7; ++i) {
            m_rcLensBtns[i] = {rlx + i * (lensW + btnGap), rcy, rlx + i * (lensW + btnGap) + lensW, rcy + 32.0f};
            bool supported = true;
            if (i == 0 && m_state.phoneMinZoom >= 1.0f) supported = false;
            if (i >= 1 && i <= 5 && lensZooms[i] > m_state.phoneMaxZoom + 0.05f) supported = false;

            bool sel = (m_state.activeLensIdx == i);
            D2D1_COLOR_F fillCol = !supported ? HexColor(0x10141C) : (sel ? COL_EMERALD : COL_BG_CARD);
            D2D1_COLOR_F brdCol  = !supported ? HexColor(0x1C222E) : (sel ? COL_EMERALD : COL_BORDER);
            D2D1_COLOR_F txtCol  = !supported ? COL_TEXT_DIM : (sel ? HexColor(0x06120C) : COL_TEXT_PRIMARY);

            DrawRoundedCard(m_rcLensBtns[i], fillCol, brdCol, 7.0f);
            DrawTextStr(lensLabels[i], m_rcLensBtns[i], m_pFontBody, txtCol, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        rcy += 38;

        // Повзунок плавного зуму (від phoneMinZoom до phoneMaxZoom, напр. 0.5x .. 10.0x)
        wchar_t zoomSliderText[96]{};
        swprintf_s(zoomSliderText, L"Плавний зум: %.1fx  (діапазон %.1fx – %.0fx)",
                   m_state.currentZoom, m_state.phoneMinZoom, m_state.phoneMaxZoom);
        DrawTextStr(zoomSliderText, {rlx, rcy, rrx, rcy + 18}, m_pFontBody, COL_CYAN);
        rcy += 20;

        m_rcZoomSlider = {rlx, rcy, rrx, rcy + 16};
        UiRect zTrack{rlx, rcy + 5, rrx, rcy + 11};
        DrawRoundedCard(zTrack, COL_BG_CARD, COL_BORDER, 3.0f);
        float zFillW = zTrack.Width() * m_state.zoomSliderNorm;
        DrawRoundedCard({rlx, rcy + 5, rlx + zFillW, rcy + 11}, COL_EMERALD, COL_EMERALD, 3.0f);
        m_pBrush->SetColor(COL_TEXT_PRIMARY);
        m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rlx + zFillW, rcy + 8.0f), 7.0f, 7.0f), m_pBrush);
        rcy += 26;

        // 4. Формат віртуальної камери (включаючи 4K UHD та 2K QHD!)
        DrawTextStr(L"4. РОЗДІЛЬНА ЗДАТНІСТЬ (ДО 4K UHD)", {rlx, rcy, rrx, rcy + 22}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 26;

        const wchar_t* resLabels[4] = {L"4K UHD", L"2K QHD", L"1080p 60", L"720p 60"};
        float resW = (rrx - rlx - 18.0f) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            m_rcResBtns[i] = {rlx + i * (resW + 6.0f), rcy, rlx + i * (resW + 6.0f) + resW, rcy + 32.0f};
            bool sel = (m_state.resPresetIdx == i);
            DrawRoundedCard(m_rcResBtns[i], sel ? COL_CYAN : COL_BG_CARD, sel ? COL_CYAN : COL_BORDER, 8.0f);
            DrawTextStr(resLabels[i], m_rcResBtns[i], m_pFontBody,
                        sel ? HexColor(0x05080D) : COL_TEXT_PRIMARY, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        rcy += 42;

        // 5. Експозиція та Колір (ISP)
        DrawTextStr(L"5. ЕКСПОЗИЦІЯ ТА КОЛІР (ISP)", {rlx, rcy, rrx, rcy + 22}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 26;

        m_rcBtnModeToggle = {rlx, rcy, rrx, rcy + 32};
        DrawRoundedCard(m_rcBtnModeToggle, m_state.isProManual ? HexColor(0x162C3D) : COL_BG_CARD,
                        m_state.isProManual ? COL_CYAN : COL_EMERALD, 8.0f);
        DrawTextStr(
            m_state.isProManual ? L"⚙ Режим: PRO MANUAL (Ручне керування)" : L"✨ Режим: AUTO ISP (Автоматичний)",
            m_rcBtnModeToggle, m_pFontBody, m_state.isProManual ? COL_CYAN : COL_EMERALD,
            DWRITE_TEXT_ALIGNMENT_CENTER
        );
        rcy += 40;

        int isoVal = 100 + static_cast<int>(m_state.isoNorm * 3100.0f);
        int shutterDenom = 30 + static_cast<int>(m_state.shutterNorm * 3970.0f);
        int wbKelvin = 2800 + static_cast<int>(m_state.wbNorm * 5700.0f);

        wchar_t sBuf0[64], sBuf1[64], sBuf2[64], sBuf3[64];
        swprintf_s(sBuf0, L"ISO Чутливість: %d", isoVal);
        swprintf_s(sBuf1, L"Витримка (Shutter): 1/%d s", shutterDenom);
        swprintf_s(sBuf2, L"Ручний фокус: %.2f dpt", m_state.focusNorm * 10.0f);
        swprintf_s(sBuf3, L"Баланс білого: %d K", wbKelvin);

        const wchar_t* sliderTitles[4] = {sBuf0, sBuf1, sBuf2, sBuf3};
        float sliderVals[4] = {m_state.isoNorm, m_state.shutterNorm, m_state.focusNorm, m_state.wbNorm};

        for (int i = 0; i < 4; ++i) {
            DrawTextStr(sliderTitles[i], {rlx, rcy, rrx, rcy + 16}, m_pFontBody,
                        m_state.isProManual ? COL_TEXT_PRIMARY : COL_TEXT_DIM);
            rcy += 18;
            m_rcSliders[i] = {rlx, rcy, rrx, rcy + 14};

            UiRect track{rlx, rcy + 4, rrx, rcy + 10};
            DrawRoundedCard(track, COL_BG_CARD, COL_BORDER, 3.0f);

            float fillW = track.Width() * sliderVals[i];
            UiRect fillRect{rlx, rcy + 4, rlx + fillW, rcy + 10};
            DrawRoundedCard(fillRect, m_state.isProManual ? COL_CYAN : COL_TEXT_DIM,
                            m_state.isProManual ? COL_CYAN : COL_TEXT_DIM, 3.0f);

            m_pBrush->SetColor(m_state.isProManual ? COL_TEXT_PRIMARY : COL_TEXT_SECOND);
            m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rlx + fillW, rcy + 7.0f), 6.5f, 6.5f), m_pBrush);

            rcy += 22;
        }

        rcy += 6;
        float halfW = (rrx - rlx - 8.0f) * 0.5f;
        m_rcBtnMirror = {rlx, rcy, rlx + halfW, rcy + 32};
        m_rcBtnGrid   = {rlx + halfW + 8.0f, rcy, rrx, rcy + 32};

        DrawRoundedCard(m_rcBtnMirror, m_state.mirrorHorizontal ? HexColor(0x162C3D) : COL_BG_CARD,
                        m_state.mirrorHorizontal ? COL_CYAN : COL_BORDER, 8.0f);
        DrawTextStr(L"⇄ Дзеркало", m_rcBtnMirror, m_pFontBody,
                    m_state.mirrorHorizontal ? COL_CYAN : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);

        DrawRoundedCard(m_rcBtnGrid, m_state.showGrid ? HexColor(0x162C3D) : COL_BG_CARD,
                        m_state.showGrid ? COL_CYAN : COL_BORDER, 8.0f);
        DrawTextStr(L"# Сітка 3x3", m_rcBtnGrid, m_pFontBody,
                    m_state.showGrid ? COL_CYAN : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);

        HRESULT hr = m_pRenderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            if (m_pPreviewBitmap) { m_pPreviewBitmap->Release(); m_pPreviewBitmap = nullptr; }
            if (m_pBrush) { m_pBrush->Release(); m_pBrush = nullptr; }
            if (m_pRenderTarget) { m_pRenderTarget->Release(); m_pRenderTarget = nullptr; }
        }
    }

    void HandleMouseDown(float x, float y) {
        // Якщо відкритий випадаючий список USB — спочатку перевіряємо вибір елемента списку
        if (m_state.transport == TransportTab::USB_ADB && m_state.usbDropdownOpen) {
            for (size_t i = 0; i < m_rcUsbItems.size(); ++i) {
                if (m_rcUsbItems[i].Contains(x, y)) {
                    m_state.selectedUsbIdx = static_cast<int>(i);
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

        // Клік по випадаючому списку USB пристроїв
        if (m_state.transport == TransportTab::USB_ADB && m_rcUsbDropdown.Contains(x, y)) {
            m_state.usbDropdownOpen = !m_state.usbDropdownOpen;
            if (m_state.usbDropdownOpen) {
                RefreshUsbDevicesAsync();
            }
            return;
        }

        // Кнопка оновлення списку USB
        if (m_state.transport == TransportTab::USB_ADB && m_rcBtnRefreshUsb.Contains(x, y)) {
            RefreshUsbDevicesAsync();
            CloseActivePhoneSocket();
            return;
        }

        // Старт / Стоп трансляції
        if (m_rcBtnStartStop.Contains(x, y)) {
            m_state.isStreaming = !m_state.isStreaming;
            if (m_state.isStreaming) {
                m_sink.Open(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2, m_state.fps);
            } else {
                CloseActivePhoneSocket();
                m_sink.Close();
            }
            return;
        }

        // Реєстрація драйвера DirectShow
        if (m_rcBtnRegisterDriver.Contains(x, y)) {
            m_state.isDriverRegistered = RegisterDirectShowDriver(m_hwnd);
            return;
        }

        // Вибір пресету зуму / об'єктива (0.5x, 1x, 2x, 3x, 5x, 10x, Front)
        const char* lensNames[7] = {"0.5x", "1.0x", "2.0x", "3.0x", "5.0x", "10.0x", "front"};
        const float lensZooms[7] = {0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f, 1.0f};
        for (int i = 0; i < 7; ++i) {
            if (m_rcLensBtns[i].Contains(x, y)) {
                if (i == 0 && m_state.phoneMinZoom >= 1.0f) return;
                if (i >= 1 && i <= 5 && lensZooms[i] > m_state.phoneMaxZoom + 0.05f) return;

                m_state.activeLensIdx = i;
                m_state.currentZoom = lensZooms[i];
                float span = (std::max)(0.1f, m_state.phoneMaxZoom - m_state.phoneMinZoom);
                m_state.zoomSliderNorm = std::clamp((m_state.currentZoom - m_state.phoneMinZoom) / span, 0.0f, 1.0f);

                char json[128]{};
                std::snprintf(json, sizeof(json), "{\"action\":\"SetCameraModule\",\"module\":\"%s\"}", lensNames[i]);
                SendControlJson(json);
                return;
            }
        }

        // Натискання на повзунок плавного зуму (до 10x)
        if (m_rcZoomSlider.Contains(x, y)) {
            m_activeSliderDrag = 100; // 100 = Zoom Slider
            UpdateZoomSliderValue(x);
            return;
        }

        // Вибір роздільної здатності (включаючи 4K UHD та 2K QHD)
        for (int i = 0; i < 4; ++i) {
            if (m_rcResBtns[i].Contains(x, y)) {
                m_state.resPresetIdx = i;
                if (i == 0)      { m_state.width = 3840; m_state.height = 2160; m_state.fps = 30; } // 4K UHD
                else if (i == 1) { m_state.width = 2560; m_state.height = 1440; m_state.fps = 60; } // 2K QHD
                else if (i == 2) { m_state.width = 1920; m_state.height = 1080; m_state.fps = 60; } // 1080p 60
                else             { m_state.width = 1280; m_state.height = 720;  m_state.fps = 60; } // 720p 60

                if (m_sink.IsOpen()) {
                    m_sink.SetFormat(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2, m_state.fps);
                }

                char json[160]{};
                std::snprintf(json, sizeof(json), "{\"action\":\"SetResolution\",\"width\":%u,\"height\":%u,\"fps\":%u}",
                              m_state.width, m_state.height, m_state.fps);
                SendControlJson(json);
                return;
            }
        }

        // Перемикач AUTO / PRO MANUAL
        if (m_rcBtnModeToggle.Contains(x, y)) {
            m_state.isProManual = !m_state.isProManual;
            SendControlJson(
                m_state.isProManual ? "{\"action\":\"set_mode\",\"mode\":\"pro\"}" : "{\"action\":\"set_mode\",\"mode\":\"auto\"}"
            );
            return;
        }

        // Перемикачі Дзеркало та Сітка
        if (m_rcBtnMirror.Contains(x, y)) {
            m_state.mirrorHorizontal = !m_state.mirrorHorizontal;
            return;
        }
        if (m_rcBtnGrid.Contains(x, y)) {
            m_state.showGrid = !m_state.showGrid;
            return;
        }

        // Перевірка натискання на повзунки ISP
        for (int i = 0; i < 4; ++i) {
            if (m_rcSliders[i].Contains(x, y)) {
                m_state.isProManual = true;
                m_activeSliderDrag = i;
                UpdateSliderValue(i, x);
                return;
            }
        }
    }

    void UpdateZoomSliderValue(float mouseX) {
        float norm = (mouseX - m_rcZoomSlider.left) / m_rcZoomSlider.Width();
        norm = std::clamp(norm, 0.0f, 1.0f);
        m_state.zoomSliderNorm = norm;

        float zoom = m_state.phoneMinZoom + norm * (m_state.phoneMaxZoom - m_state.phoneMinZoom);
        m_state.currentZoom = std::round(zoom * 10.0f) / 10.0f;

        char json[128]{};
        std::snprintf(json, sizeof(json), "{\"action\":\"SetZoom\",\"zoom\":%.2f}", m_state.currentZoom);
        SendControlJson(json);
    }

    void UpdateSliderValue(int idx, float mouseX) {
        if (idx < 0 || idx >= 4) return;
        const UiRect& r = m_rcSliders[idx];
        float norm = std::clamp((mouseX - r.left) / r.Width(), 0.0f, 1.0f);

        char json[160]{};
        if (idx == 0) {
            m_state.isoNorm = norm;
            int iso = 100 + static_cast<int>(norm * 3100.0f);
            std::snprintf(json, sizeof(json), "{\"action\":\"SetISO\",\"auto\":false,\"iso\":%d}", iso);
        } else if (idx == 1) {
            m_state.shutterNorm = norm;
            int denom = 30 + static_cast<int>(norm * 3970.0f);
            long long expNs = 1000000000LL / denom;
            std::snprintf(json, sizeof(json), "{\"action\":\"SetShutter\",\"auto\":false,\"exposure_ns\":%lld}", expNs);
        } else if (idx == 2) {
            m_state.focusNorm = norm;
            std::snprintf(json, sizeof(json), "{\"action\":\"SetFocus\",\"mode\":\"MANUAL\",\"distance_diopters\":%.2f}", norm * 10.0f);
        } else if (idx == 3) {
            m_state.wbNorm = norm;
            int kelvin = 2800 + static_cast<int>(norm * 5700.0f);
            std::snprintf(json, sizeof(json), "{\"action\":\"SetWhiteBalance\",\"mode\":\"MANUAL_KELVIN\",\"kelvin\":%d}", kelvin);
        }
        SendControlJson(json);
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
            case WM_SIZE:
                if (self->m_pRenderTarget) {
                    self->m_pRenderTarget->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
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

            case WM_MOUSEMOVE:
                if (self->m_activeSliderDrag == 100 && (wParam & MK_LBUTTON)) {
                    self->UpdateZoomSliderValue(static_cast<float>(GET_X_LPARAM(lParam)));
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (self->m_activeSliderDrag >= 0 && (wParam & MK_LBUTTON)) {
                    self->UpdateSliderValue(
                        self->m_activeSliderDrag,
                        static_cast<float>(GET_X_LPARAM(lParam))
                    );
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;

            case WM_LBUTTONUP:
                ReleaseCapture();
                self->m_activeSliderDrag = -1;
                return 0;

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
    ULONG_PTR m_gdiplusToken = 0;
    ID2D1Factory* m_pD2DFactory = nullptr;
    IDWriteFactory* m_pDWriteFactory = nullptr;
    ID2D1HwndRenderTarget* m_pRenderTarget = nullptr;
    ID2D1SolidColorBrush* m_pBrush = nullptr;
    ID2D1Bitmap* m_pPreviewBitmap = nullptr;

    IDWriteTextFormat* m_pFontTitle = nullptr;
    IDWriteTextFormat* m_pFontSection = nullptr;
    IDWriteTextFormat* m_pFontBody = nullptr;
    IDWriteTextFormat* m_pFontMono = nullptr;

    StudioState m_state{};
    EyeCam::WindowsDirectShowSink m_sink{};

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_usbScanInProgress{false};
    std::mutex m_socketMutex;
    std::mutex m_usbMutex;
    std::mutex m_frameMutex;
    SOCKET m_phoneSocket = INVALID_SOCKET;
    std::thread m_streamThread;

    uint32_t m_frameTick = 0;
    bool m_hasLiveFrame = false;
    std::vector<uint8_t> m_previewPixels;
    std::vector<uint8_t> m_yuy2Buffer;

    // Інтерактивні зони UI
    UiRect m_rcTransportTabs[2]{};
    UiRect m_rcIpInput{};
    UiRect m_rcUsbDropdown{};
    UiRect m_rcBtnRefreshUsb{};
    std::vector<UiRect> m_rcUsbItems;
    UiRect m_rcBtnStartStop{};
    UiRect m_rcBtnRegisterDriver{};
    UiRect m_rcLensBtns[7]{};
    UiRect m_rcZoomSlider{};
    UiRect m_rcResBtns[4]{};
    UiRect m_rcBtnModeToggle{};
    UiRect m_rcSliders[4]{};
    UiRect m_rcBtnMirror{};
    UiRect m_rcBtnGrid{};
    int m_activeSliderDrag = -1;
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
