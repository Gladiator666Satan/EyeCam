#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <commctrl.h>

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cmath>

#include "WindowsDirectShowSink.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")

namespace {

// ============================================================================
// Фірмові кольори Win11 Fluent Dark для EyeCam
// ============================================================================
constexpr D2D1_COLOR_F COL_BG_DARK       = {0.047f, 0.055f, 0.071f, 1.0f}; // #0C0E12
constexpr D2D1_COLOR_F COL_BG_CARD       = {0.082f, 0.098f, 0.125f, 1.0f}; // #151920
constexpr D2D1_COLOR_F COL_BG_CARD_HOVER = {0.118f, 0.137f, 0.176f, 1.0f}; // #1E232D
constexpr D2D1_COLOR_F COL_BORDER        = {0.157f, 0.180f, 0.224f, 1.0f}; // #282E39
constexpr D2D1_COLOR_F COL_EMERALD       = {0.063f, 0.725f, 0.506f, 1.0f}; // #10B981
constexpr D2D1_COLOR_F COL_CYAN          = {0.024f, 0.710f, 0.831f, 1.0f}; // #06B6D4
constexpr D2D1_COLOR_F COL_TEXT_PRIMARY  = {0.941f, 0.949f, 0.961f, 1.0f}; // #F0F2F5
constexpr D2D1_COLOR_F COL_TEXT_MUTED    = {0.580f, 0.620f, 0.690f, 1.0f}; // #949EB0
constexpr D2D1_COLOR_F COL_TEXT_DIM      = {0.380f, 0.420f, 0.480f, 1.0f}; // #616B7A
constexpr D2D1_COLOR_F COL_RED           = {0.937f, 0.267f, 0.267f, 1.0f}; // #EF4444

inline D2D1_COLOR_F HexColor(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(
        static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgb & 0xFF) / 255.0f,
        a
    );
}

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int count = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()), nullptr, 0);
    std::wstring out(count, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()), &out[0], count);
    return out;
}

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int count = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()), nullptr, 0, nullptr, nullptr);
    std::string out(count, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()), &out[0], count, nullptr, nullptr);
    return out;
}

struct UiRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    bool Contains(float x, float y) const {
        return x >= left && x <= right && y >= top && y <= bottom;
    }
    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
};

enum class TransportTab {
    USB_ADB = 0,
    WIFI_LAN = 1
};

struct UsbDeviceItem {
    std::string serial;
    std::string model;
    std::string state;
};

struct StudioState {
    TransportTab transport = TransportTab::USB_ADB;

    // USB ADB стан
    std::vector<UsbDeviceItem> usbDevices;
    int selectedUsbIdx = -1;
    bool usbDropdownOpen = false;

    // Wi-Fi LAN стан
    std::string deviceIp = "192.168.1.100";
    uint16_t devicePort = 8554;
    bool ipInputFocused = false;

    // Стан віртуальної камери DirectShow
    bool isStreaming = false;
    bool isDirectShowRegistered = false;
    bool isPhoneConnected = false;
    std::wstring connectedPhoneName = L"Не підключено";

    // Налаштування роздільної здатності (до 4K UHD!)
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    int resPresetIdx = 2; // 0=4K, 1=2K, 2=1080p, 3=720p

    // Оптика та Зум (підтримка до 10x на флагманах на кшталт S26 Ultra)
    float phoneMinZoom = 0.5f;
    float phoneMaxZoom = 10.0f;
    float currentZoom = 1.0f;
    int activeLensIdx = 1; // 0=0.5x, 1=1x, 2=2x, 3=3x, 4=5x, 5=10x, 6=Front
    float zoomSliderNorm = 0.05f; // Плавний зум від phoneMinZoom до phoneMaxZoom

    // Ручний Pro ISP режим
    bool isProManual = false;
    float isoNorm = 0.2f;       // 100 .. 3200
    float shutterNorm = 0.3f;   // 1/30 .. 1/4000
    float focusNorm = 0.5f;     // 0 .. 10 D
    float wbNorm = 0.5f;        // 2800K .. 8500K

    // Ефекти та композиція
    bool mirrorHorizontal = false;
    bool showFramingGrid = false;

    // Мережева телеметрія
    uint32_t measuredBitrateKbps = 0;
    uint32_t measuredFps = 0;
    uint64_t rxBytes = 0;
    uint64_t rxFrames = 0;
};

// ============================================================================
// Допоміжні функції сканування USB ADB пристроїв
// ============================================================================
std::string ExecuteCommandSilent(const std::string& cmd) {
    std::string result;
    HANDLE hReadPipe, hWritePipe;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return "";
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;

    PROCESS_INFORMATION pi{};
    char cmdBuf[1024];
    strncpy_s(cmdBuf, cmd.c_str(), sizeof(cmdBuf) - 1);

    if (CreateProcessA(nullptr, cmdBuf, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWritePipe);
        char buf[512];
        DWORD bytesRead = 0;
        while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buf[bytesRead] = '\0';
            result += buf;
        }
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        CloseHandle(hWritePipe);
    }
    CloseHandle(hReadPipe);
    return result;
}

std::vector<UsbDeviceItem> ScanConnectedAdbDevices() {
    std::vector<UsbDeviceItem> list;
    std::string out = ExecuteCommandSilent("adb devices -l");
    std::istringstream stream(out);
    std::string line;

    bool headerPassed = false;
    while (std::getline(stream, line)) {
        if (!headerPassed) {
            if (line.find("List of devices attached") != std::string::npos) {
                headerPassed = true;
            }
            continue;
        }
        if (line.empty() || line[0] == '\r') continue;

        std::istringstream lineStream(line);
        std::string serial, state;
        if (lineStream >> serial >> state) {
            std::string model = "Android Device";
            std::string token;
            while (lineStream >> token) {
                if (token.rfind("model:", 0) == 0) {
                    model = token.substr(6);
                    std::replace(model.begin(), model.end(), '_', ' ');
                }
            }
            list.push_back({serial, model, state});
        }
    }
    return list;
}

bool ForwardAdbPort(const std::string& serial, uint16_t port) {
    std::string cmd = "adb ";
    if (!serial.empty()) {
        cmd += "-s " + serial + " ";
    }
    cmd += "forward tcp:" + std::to_string(port) + " tcp:" + std::to_string(port);
    std::string out = ExecuteCommandSilent(cmd);
    return out.find("error") == std::string::npos;
}

// ============================================================================
// Перевірка та реєстрація DirectShow фільтра віртуальної камери
// ============================================================================
bool CheckDirectShowDriverRegistered() {
    HKEY hKey = nullptr;
    // CLSID віртуальної камери EyeCam DirectShow Filter:
    // {8A9E1983-4712-4C10-B180-22D2C2081985}
    const wchar_t* subkey = L"CLSID\\{8A9E1983-4712-4C10-B180-22D2C2081985}";
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, subkey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

bool RegisterDirectShowDriver() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t lastSlash = dir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        dir = dir.substr(0, lastSlash);
    }
    std::wstring dllPath = dir + L"\\EyeCamFilter.dll";

    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Також шукаємо в батьківській папці або build
        dllPath = dir + L"\\bin\\EyeCamFilter.dll";
    }

    std::wstring cmd = L"regsvr32.exe /s \"" + dllPath + L"\"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas"; // Вимагає UAC підвищення прав для реєстрації COM фільтра
    sei.lpFile = L"regsvr32.exe";
    std::wstring params = L"/s \"" + dllPath + L"\"";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;

    if (ShellExecuteExW(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 5000);
        CloseHandle(sei.hProcess);
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

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProcThunk;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"EyeCamStudioWin11Class";

        // Програмна генерація іконки: стилізоване око з бліком об'єктиву у рамці монітора
        HICON hIcon = GenerateEyeCamWindowIcon();
        wc.hIcon = hIcon;
        wc.hIconSm = hIcon;

        if (!RegisterClassExW(&wc)) return false;

        const int w = 1180;
        const int h = 760;
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int posX = (screenW - w) / 2;
        int posY = (screenH - h) / 2;

        m_hWnd = CreateWindowExW(
            WS_EX_APPWINDOW,
            wc.lpszClassName,
            L"EyeCam Studio — Віртуальна 4K веб-камера нового покоління",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            posX, posY, w, h,
            nullptr, nullptr, hInstance, this
        );

        if (!m_hWnd) return false;

        if (!InitDirect2D()) return false;

        m_state.isDirectShowRegistered = CheckDirectShowDriverRegistered();

        // Початкове сканування USB пристроїв
        RefreshUsbDeviceListAsync();

        m_running = true;
        m_streamThread = std::thread(&EyeCamStudioWindow::PhoneStreamClientLoop, this);

        ShowWindow(m_hWnd, nCmdShow);
        UpdateWindow(m_hWnd);

        SetTimer(m_hWnd, 1, 16, nullptr); // ~60 кадрів/сек для плавної анімації інтерфейсу
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
    HWND m_hWnd = nullptr;
    ID2D1Factory* m_pD2DFactory = nullptr;
    ID2D1HwndRenderTarget* m_pRenderTarget = nullptr;
    IDWriteFactory* m_pDWriteFactory = nullptr;
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
    std::vector<uint8_t> m_fullBgraBuffer;
    std::vector<uint8_t> m_cachedPreviewBuffer;

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
    UiRect m_rcBtnRefocus{};
    UiRect m_rcSliders[4]{};
    UiRect m_rcBtnMirror{};
    UiRect m_rcBtnGrid{};
    int m_activeSliderDrag = -1;

    // ========================================================================
    // Генерація системної іконки вікна: рамка ноутбука + діафрагма ока з бліком
    // ========================================================================
    HICON GenerateEyeCamWindowIcon() {
        const int s = 32;
        HDC hdc = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP hBitmap = CreateCompatibleBitmap(hdc, s, s);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(memDC, hBitmap);

        HBRUSH bgBrush = CreateSolidBrush(RGB(12, 14, 18));
        RECT r{0, 0, s, s};
        FillRect(memDC, &r, bgBrush);
        DeleteObject(bgBrush);

        // Рамка екрана / ноутбука
        HPEN framePen = CreatePen(PS_SOLID, 2, RGB(40, 46, 57));
        HPEN oldPen = (HPEN)SelectObject(memDC, framePen);
        SelectObject(memDC, GetStockObject(NULL_BRUSH));
        RoundRect(memDC, 2, 3, 30, 25, 4, 4);

        // Підставка ноутбука
        MoveToEx(memDC, 5, 27, nullptr);
        LineTo(memDC, 27, 27);
        MoveToEx(memDC, 12, 28, nullptr);
        LineTo(memDC, 20, 28);
        SelectObject(memDC, oldPen);
        DeleteObject(framePen);

        // Зовнішній контур ока
        HBRUSH cyanBrush = CreateSolidBrush(RGB(6, 182, 212));
        HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, cyanBrush);
        Ellipse(memDC, 6, 7, 26, 21);
        SelectObject(memDC, oldBrush);
        DeleteObject(cyanBrush);

        // Зіниця ока (об'єктив)
        HBRUSH darkBrush = CreateSolidBrush(RGB(8, 12, 18));
        SelectObject(memDC, darkBrush);
        Ellipse(memDC, 11, 9, 21, 19);
        SelectObject(memDC, oldBrush);
        DeleteObject(darkBrush);

        // Блік лінзи об'єктива
        HBRUSH flareBrush = CreateSolidBrush(RGB(240, 250, 255));
        SelectObject(memDC, flareBrush);
        Ellipse(memDC, 13, 11, 16, 14);
        SelectObject(memDC, oldBrush);
        DeleteObject(flareBrush);

        SelectObject(memDC, hOldBmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, hdc);

        HBITMAP hMask = CreateBitmap(s, s, 1, 1, nullptr);
        ICONINFO ii{};
        ii.fIcon = TRUE;
        ii.hbmColor = hBitmap;
        ii.hbmMask = hMask;

        HICON icon = CreateIconIndirect(&ii);
        DeleteObject(hBitmap);
        DeleteObject(hMask);
        return icon;
    }

    bool InitDirect2D() {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pD2DFactory);
        if (FAILED(hr)) return false;

        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&m_pDWriteFactory)
        );
        if (FAILED(hr)) return false;

        RECT rc{};
        GetClientRect(m_hWnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

        hr = m_pD2DFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(m_hWnd, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY),
            &m_pRenderTarget
        );
        if (FAILED(hr)) return false;

        m_pRenderTarget->CreateSolidColorBrush(COL_TEXT_PRIMARY, &m_pBrush);

        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI Variable Display", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            18.0f, L"uk-UA", &m_pFontTitle
        );

        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            13.0f, L"uk-UA", &m_pFontSection
        );

        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"uk-UA", &m_pFontBody
        );

        m_pDWriteFactory->CreateTextFormat(
            L"Cascadia Code", nullptr,
            DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            11.5f, L"en-US", &m_pFontMono
        );

        return true;
    }

    void Shutdown() {
        m_running = false;
        CloseActivePhoneSocket();
        if (m_streamThread.joinable()) {
            m_streamThread.join();
        }
        if (m_sink.IsOpen()) {
            m_sink.Close();
        }

        if (m_pPreviewBitmap) { m_pPreviewBitmap->Release(); m_pPreviewBitmap = nullptr; }
        if (m_pBrush) { m_pBrush->Release(); m_pBrush = nullptr; }
        if (m_pFontTitle) { m_pFontTitle->Release(); m_pFontTitle = nullptr; }
        if (m_pFontSection) { m_pFontSection->Release(); m_pFontSection = nullptr; }
        if (m_pFontBody) { m_pFontBody->Release(); m_pFontBody = nullptr; }
        if (m_pFontMono) { m_pFontMono->Release(); m_pFontMono = nullptr; }
        if (m_pRenderTarget) { m_pRenderTarget->Release(); m_pRenderTarget = nullptr; }
        if (m_pDWriteFactory) { m_pDWriteFactory->Release(); m_pDWriteFactory = nullptr; }
        if (m_pD2DFactory) { m_pD2DFactory->Release(); m_pD2DFactory = nullptr; }

        WSACleanup();
    }

    void CloseActivePhoneSocket() {
        std::lock_guard<std::mutex> lock(m_socketMutex);
        if (m_phoneSocket != INVALID_SOCKET) {
            closesocket(m_phoneSocket);
            m_phoneSocket = INVALID_SOCKET;
        }
    }

    void RefreshUsbDeviceListAsync() {
        if (m_usbScanInProgress) return;
        m_usbScanInProgress = true;

        std::thread([this]() {
            auto devices = ScanConnectedAdbDevices();
            {
                std::lock_guard<std::mutex> lock(m_usbMutex);
                m_state.usbDevices = std::move(devices);
                if (m_state.selectedUsbIdx < 0 || m_state.selectedUsbIdx >= static_cast<int>(m_state.usbDevices.size())) {
                    m_state.selectedUsbIdx = m_state.usbDevices.empty() ? -1 : 0;
                }
            }
            m_usbScanInProgress = false;
            InvalidateRect(m_hWnd, nullptr, FALSE);
        }).detach();
    }

    void SendControlJson(const std::string& jsonPayload) {
        std::thread([this, jsonPayload]() {
            uint8_t header[8]{};
            header[0] = 0x43;
            header[1] = 0x50;
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
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* pWicFactory = nullptr;
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&pWicFactory)
        );

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
                    DecodeIncomingJpegFrame(pWicFactory, payload.data(), payload.size());
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

        if (pWicFactory) {
            pWicFactory->Release();
        }
        CoUninitialize();
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

    // Декодування живого JPEG-кадру зі смартфона через WIC у BGRA прев'ю та YUY2 буфер DirectShow віртуальної камери
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
                    // 1. Повнорозмірне декодування для системної віртуальної камери DirectShow
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
                                PushFrameToDirectShow(m_fullBgraBuffer.data(), imgW, imgH);

                                // 2. Швидкий прямокутний даунсемплінг у 640x360 для UI прев'ю без другого циклу WIC (< 0.2 мс)
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

        const uint32_t outW = m_state.width;
        const uint32_t outH = m_state.height;
        size_t expectedYuy2 = EyeCam::CalculateFrameSize(outW, outH, EyeCam::PixelFormat::YUY2);
        if (m_yuy2Buffer.size() != expectedYuy2) {
            m_yuy2Buffer.resize(expectedYuy2);
        }

        for (uint32_t y = 0; y < outH; ++y) {
            uint32_t srcY = (y * inH) / outH;
            const uint8_t* srcRow = srcBgra + (srcY * inW * 4);
            uint8_t* dstRow = m_yuy2Buffer.data() + (y * outW * 2);

            for (uint32_t x = 0; x < outW; x += 2) {
                uint32_t srcX = (x * inW) / outW;
                const uint8_t* p = srcRow + (srcX * 4);
                uint8_t b = p[0];
                uint8_t g = p[1];
                uint8_t r = p[2];

                int y0 = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

                uint32_t srcX2 = ((x + 1) * inW) / outW;
                const uint8_t* p2 = srcRow + (srcX2 * 4);
                int y1 = ((66 * p2[2] + 129 * p2[1] + 25 * p2[0] + 128) >> 8) + 16;

                dstRow[x * 2 + 0] = static_cast<uint8_t>(std::clamp(y0, 0, 255));
                dstRow[x * 2 + 1] = static_cast<uint8_t>(std::clamp(u, 0, 255));
                dstRow[x * 2 + 2] = static_cast<uint8_t>(std::clamp(y1, 0, 255));
                dstRow[x * 2 + 3] = static_cast<uint8_t>(std::clamp(v, 0, 255));
            }
        }

        if (m_sink.IsOpen()) {
            m_sink.PushFrame(m_yuy2Buffer.data(), m_yuy2Buffer.size(), EyeCam::PixelFormat::YUY2);
        }
    }

    // ========================================================================
    // Отрисовка інтерфейсу EyeCam Studio (Direct2D Win11 Fluent Dark)
    // ========================================================================
    void Render() {
        if (!m_pRenderTarget) return;

        m_pRenderTarget->BeginDraw();
        m_pRenderTarget->Clear(COL_BG_DARK);

        RECT rcClient{};
        GetClientRect(m_hWnd, &rcClient);
        float width = static_cast<float>(rcClient.right);
        float height = static_cast<float>(rcClient.bottom);

        // 1. Верхній Fluent Header (Логотип, Заголовок, Статус драйвера віртуальної камери)
        DrawHeader(width);

        // 2. Ліва колонка: Велике 16:9 вікно попереднього перегляду камери + Телеметрія потоку
        DrawPreviewSection(24.0f, 68.0f, 650.0f, height - 88.0f);

        // 3. Права колонка: Налаштування з'єднання, оптика (0.5x..10x), 4K резолюція, ISP та ефекти
        DrawControlsPanel(694.0f, 68.0f, width - 24.0f, height - 24.0f);

        // 4. Оверлей випадного списку USB пристроїв, якщо він розгорнутий
        if (m_state.usbDropdownOpen && m_state.transport == TransportTab::USB_ADB) {
            DrawUsbDropdownOverlay();
        }

        m_pRenderTarget->EndDraw();
    }

    void DrawHeader(float width) {
        m_pBrush->SetColor(COL_BG_CARD);
        m_pRenderTarget->FillRectangle(D2D1::RectF(0.0f, 0.0f, width, 54.0f), m_pBrush);

        m_pBrush->SetColor(COL_BORDER);
        m_pRenderTarget->DrawLine(D2D1::Point2F(0.0f, 54.0f), D2D1::Point2F(width, 54.0f), m_pBrush, 1.0f);

        // Логотип: синє стилізоване око з бліком
        DrawLogoIcon(24.0f, 13.0f);

        // Заголовок програми
        DrawTextStr(L"EYECAM STUDIO", {62.0f, 10.0f, 260.0f, 32.0f}, m_pFontTitle, COL_TEXT_PRIMARY);
        DrawTextStr(L"Win11 Fluent Dark • 4K UHD DirectShow Virtual Camera", {62.0f, 32.0f, 440.0f, 50.0f}, m_pFontMono, COL_TEXT_MUTED);

        // Статус DirectShow фільтра віртуальної камери в системі
        float btnW = 230.0f;
        m_rcBtnRegisterDriver = {width - btnW - 24.0f, 12.0f, width - 24.0f, 42.0f};

        bool reg = m_state.isDirectShowRegistered;
        DrawRoundedCard(
            m_rcBtnRegisterDriver,
            reg ? HexColor(0x0E261D) : HexColor(0x2D1515),
            reg ? COL_EMERALD : COL_RED,
            6.0f
        );

        std::wstring driverStatusText = reg ? L"✔ DirectShow Камера: АКТИВНА" : L"⚠ Зареєструвати драйвер камери";
        DrawTextStr(
            driverStatusText.c_str(),
            m_rcBtnRegisterDriver,
            m_pFontBody,
            reg ? COL_EMERALD : HexColor(0xFCA5A5),
            DWRITE_TEXT_ALIGNMENT_CENTER
        );
    }

    void DrawLogoIcon(float x, float y) {
        // Рамка ноутбука навколо ока
        m_pBrush->SetColor(COL_BORDER);
        m_pRenderTarget->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x - 2.0f, y - 2.0f, x + 30.0f, y + 26.0f), 4.0f, 4.0f),
            m_pBrush, 1.5f
        );
        m_pRenderTarget->DrawLine(D2D1::Point2F(x + 2.0f, y + 29.0f), D2D1::Point2F(x + 26.0f, y + 29.0f), m_pBrush, 2.0f);

        // Зовнішній контур ока
        m_pBrush->SetColor(COL_CYAN);
        m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 14.0f, y + 12.0f), 13.0f, 8.5f), m_pBrush);

        // Зіниця (об'єктив)
        m_pBrush->SetColor(COL_BG_DARK);
        m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 14.0f, y + 12.0f), 5.5f, 5.5f), m_pBrush);

        // Блік лінзи об'єктива
        m_pBrush->SetColor(COL_TEXT_PRIMARY);
        m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 12.0f, y + 10.0f), 1.8f, 1.8f), m_pBrush);
    }

    void DrawPreviewSection(float left, float top, float right, float bottom) {
        float previewW = right - left;
        float previewH = previewW * (9.0f / 16.0f); // 16:9 widescreen preview

        UiRect rcPreviewBox{left, top, right, top + previewH};
        DrawRoundedCard(rcPreviewBox, HexColor(0x06080A), COL_BORDER, 10.0f);

        // Відмальовка живого кадру зі смартфона (WIC BGRA -> Direct2D Bitmap)
        bool renderedLive = false;
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            if (m_hasLiveFrame && !m_previewPixels.empty()) {
                const uint32_t pw = 640;
                const uint32_t ph = 360;
                if (!m_pPreviewBitmap) {
                    D2D1_BITMAP_PROPERTIES props{};
                    props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
                    m_pRenderTarget->CreateBitmap(D2D1::SizeU(pw, ph), props, &m_pPreviewBitmap);
                }
                if (m_pPreviewBitmap) {
                    D2D1_RECT_U destRect{0, 0, pw, ph};
                    m_pPreviewBitmap->CopyFromMemory(&destRect, m_previewPixels.data(), pw * 4);

                    D2D1_RECT_F drawRect = D2D1::RectF(left + 2.0f, top + 2.0f, right - 2.0f, top + previewH - 2.0f);
                    m_pRenderTarget->DrawBitmap(m_pPreviewBitmap, drawRect);
                    renderedLive = true;
                }
            }
        }

        if (!renderedLive) {
            // Заглушка, коли смартфон ще не транслює потік
            std::wstring statusMsg = m_state.isStreaming
                ? L"Очікування підключення смартфона EyeCam...\n(Перевірте, щоб додаток на телефоні був відкритий)"
                : L"Трансляцію зупинено\nОберіть USB або Wi-Fi та натисніть «Запустити віртуальну камеру»";

            DrawTextStr(statusMsg.c_str(), rcPreviewBox, m_pFontBody, COL_TEXT_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // Сітка візирного правила третин (якщо увімкнена)
        if (m_state.showFramingGrid) {
            m_pBrush->SetColor(HexColor(0xFFFFFF, 0.15f));
            float gw = rcPreviewBox.Width();
            float gh = rcPreviewBox.Height();
            m_pRenderTarget->DrawLine(D2D1::Point2F(left + gw / 3.0f, top), D2D1::Point2F(left + gw / 3.0f, top + gh), m_pBrush, 1.0f);
            m_pRenderTarget->DrawLine(D2D1::Point2F(left + 2.0f * gw / 3.0f, top), D2D1::Point2F(left + 2.0f * gw / 3.0f, top + gh), m_pBrush, 1.0f);
            m_pRenderTarget->DrawLine(D2D1::Point2F(left, top + gh / 3.0f), D2D1::Point2F(right, top + gh / 3.0f), m_pBrush, 1.0f);
            m_pRenderTarget->DrawLine(D2D1::Point2F(left, top + 2.0f * gh / 3.0f), D2D1::Point2F(right, top + 2.0f * gh / 3.0f), m_pBrush, 1.0f);
        }

        // Бейдж поточної роздільної здатності та FPS у кутку прев'ю
        UiRect rcBadge{left + 12.0f, top + 12.0f, left + 200.0f, top + 36.0f};
        DrawRoundedCard(rcBadge, HexColor(0x0C0E12, 0.85f), COL_BORDER, 6.0f);
        wchar_t resBadgeStr[64]{};
        swprintf_s(resBadgeStr, L"● %ux%u @ %u FPS", m_state.width, m_state.height, m_state.fps);
        DrawTextStr(resBadgeStr, rcBadge, m_pFontMono, m_state.isPhoneConnected ? COL_EMERALD : COL_TEXT_MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);

        // Панель інструментів швидкої дії над прев'ю (Дзеркало, Сітка)
        m_rcBtnMirror = {right - 140.0f, top + 12.0f, right - 76.0f, top + 36.0f};
        DrawRoundedCard(m_rcBtnMirror, m_state.mirrorHorizontal ? HexColor(0x1B3028) : HexColor(0x0C0E12, 0.85f),
                        m_state.mirrorHorizontal ? COL_EMERALD : COL_BORDER, 6.0f);
        DrawTextStr(L"↔ Дзеркало", m_rcBtnMirror, m_pFontBody, m_state.mirrorHorizontal ? COL_EMERALD : COL_TEXT_MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);

        m_rcBtnGrid = {right - 68.0f, top + 12.0f, right - 12.0f, top + 36.0f};
        DrawRoundedCard(m_rcBtnGrid, m_state.showFramingGrid ? HexColor(0x1B3028) : HexColor(0x0C0E12, 0.85f),
                        m_state.showFramingGrid ? COL_EMERALD : COL_BORDER, 6.0f);
        DrawTextStr(L"# Сітка", m_rcBtnGrid, m_pFontBody, m_state.showFramingGrid ? COL_EMERALD : COL_TEXT_MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);

        // Картка живої телеметрії потоку
        float teleTop = top + previewH + 16.0f;
        UiRect rcTeleCard{left, teleTop, right, bottom};
        DrawRoundedCard(rcTeleCard, COL_BG_CARD, COL_BORDER, 10.0f);

        DrawTextStr(L"ТЕЛЕМЕТРІЯ ТА СТАТУС ПІДКЛЮЧЕННЯ", {left + 18.0f, teleTop + 14.0f, right - 18.0f, teleTop + 32.0f}, m_pFontSection, COL_TEXT_PRIMARY);

        // Рядок 1: Підключений телефон
        std::wstring phoneStatus = L"Пристрій: " + m_state.connectedPhoneName;
        DrawTextStr(phoneStatus.c_str(), {left + 18.0f, teleTop + 42.0f, right - 18.0f, teleTop + 62.0f}, m_pFontBody, COL_TEXT_MUTED);

        // Рядок 2: Мережевий потік, бітрейт та реальний FPS
        wchar_t netStat[160]{};
        float mbTotal = static_cast<float>(m_state.rxBytes) / (1024.0f * 1024.0f);
        swprintf_s(netStat, L"Бітрейт: %.1f Мбіт/с   •   Реальний FPS: %u   •   Отримано: %.1f МБ (%llu кадрів)",
                   static_cast<float>(m_state.measuredBitrateKbps) / 1000.0f,
                   m_state.measuredFps, mbTotal, m_state.rxFrames);
        DrawTextStr(netStat, {left + 18.0f, teleTop + 68.0f, right - 18.0f, teleTop + 88.0f}, m_pFontMono, COL_CYAN);

        // Рядок 3: Канал передачі
        std::wstring chanStr = (m_state.transport == TransportTab::USB_ADB)
            ? L"Канал: Високошвидкісний USB ADB (127.0.0.1:8554 • нульова затримка)"
            : L"Канал: Бездротова мережа Wi-Fi LAN (" + Utf8ToWide(m_state.deviceIp) + L":8554)";
        DrawTextStr(chanStr.c_str(), {left + 18.0f, teleTop + 94.0f, right - 18.0f, teleTop + 114.0f}, m_pFontBody, COL_TEXT_MUTED);
    }

    void DrawControlsPanel(float left, float top, float right, float bottom) {
        UiRect rcCard{left, top, right, bottom};
        DrawRoundedCard(rcCard, COL_BG_CARD, COL_BORDER, 10.0f);

        float rlx = left + 18.0f;
        float rrx = right - 18.0f;
        float rcy = top + 18.0f;

        // 1. Вкладки вибору транспорту (USB ADB проти Wi-Fi LAN)
        DrawTextStr(L"1. ДЖЕРЕЛО З'ЄДНАННЯ З ТЕЛЕФОНОМ", {rlx, rcy, rrx, rcy + 22}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 28;

        float tabW = (rrx - rlx - 8.0f) / 2.0f;
        m_rcTransportTabs[0] = {rlx, rcy, rlx + tabW, rcy + 34.0f};
        m_rcTransportTabs[1] = {rlx + tabW + 8.0f, rcy, rrx, rcy + 34.0f};

        bool usbSel = (m_state.transport == TransportTab::USB_ADB);
        DrawRoundedCard(m_rcTransportTabs[0], usbSel ? HexColor(0x133026) : COL_BG_DARK, usbSel ? COL_EMERALD : COL_BORDER, 6.0f);
        DrawTextStr(L"🔌 USB ADB (Рекомендовано)", m_rcTransportTabs[0], m_pFontBody, usbSel ? COL_EMERALD : COL_TEXT_MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);

        DrawRoundedCard(m_rcTransportTabs[1], !usbSel ? HexColor(0x133026) : COL_BG_DARK, !usbSel ? COL_EMERALD : COL_BORDER, 6.0f);
        DrawTextStr(L"📶 Wi-Fi Бездротовий LAN", m_rcTransportTabs[1], m_pFontBody, !usbSel ? COL_EMERALD : COL_TEXT_MUTED, DWRITE_TEXT_ALIGNMENT_CENTER);
        rcy += 42;

        if (m_state.transport == TransportTab::USB_ADB) {
            // Випадаючий список виявлених USB пристроїв
            DrawTextStr(L"Підключений USB-смартфон:", {rlx, rcy, rrx, rcy + 18}, m_pFontBody, COL_TEXT_MUTED);
            rcy += 22;

            float refreshBtnW = 110.0f;
            m_rcUsbDropdown = {rlx, rcy, rrx - refreshBtnW - 8.0f, rcy + 34.0f};
            m_rcBtnRefreshUsb = {rrx - refreshBtnW, rcy, rrx, rcy + 34.0f};

            DrawRoundedCard(m_rcUsbDropdown, COL_BG_DARK, COL_BORDER, 6.0f);

            std::wstring curDevStr = L"Пошук пристроїв...";
            {
                std::lock_guard<std::mutex> lock(m_usbMutex);
                if (m_state.usbDevices.empty()) {
                    curDevStr = m_usbScanInProgress ? L"⏳ Сканування USB шини..." : L"❌ Пристроїв не знайдено (увімкніть налагодження USB)";
                } else if (m_state.selectedUsbIdx >= 0 && m_state.selectedUsbIdx < static_cast<int>(m_state.usbDevices.size())) {
                    const auto& d = m_state.usbDevices[m_state.selectedUsbIdx];
                    curDevStr = Utf8ToWide(d.model) + L" (" + Utf8ToWide(d.serial) + L")";
                }
            }

            DrawTextStr(curDevStr.c_str(), {m_rcUsbDropdown.left + 12.0f, rcy + 8.0f, m_rcUsbDropdown.right - 28.0f, rcy + 28.0f}, m_pFontBody, COL_TEXT_PRIMARY);
            DrawTextStr(L"▼", {m_rcUsbDropdown.right - 24.0f, rcy + 8.0f, m_rcUsbDropdown.right - 8.0f, rcy + 28.0f}, m_pFontBody, COL_TEXT_MUTED);

            // Кнопка «Оновити USB»
            DrawRoundedCard(m_rcBtnRefreshUsb, HexColor(0x1B2330), COL_BORDER, 6.0f);
            DrawTextStr(L"🔄 Оновити", m_rcBtnRefreshUsb, m_pFontBody, COL_TEXT_PRIMARY, DWRITE_TEXT_ALIGNMENT_CENTER);
            rcy += 44;
        } else {
            // Введення IP адреси телефону
            DrawTextStr(L"IP-адреса смартфона у локальній мережі:", {rlx, rcy, rrx, rcy + 18}, m_pFontBody, COL_TEXT_MUTED);
            rcy += 22;

            m_rcIpInput = {rlx, rcy, rrx, rcy + 34.0f};
            DrawRoundedCard(m_rcIpInput, COL_BG_DARK, m_state.ipInputFocused ? COL_CYAN : COL_BORDER, 6.0f);
            std::wstring ipW = Utf8ToWide(m_state.deviceIp) + (m_state.ipInputFocused ? L"|" : L"");
            DrawTextStr(ipW.c_str(), {rlx + 12.0f, rcy + 8.0f, rrx - 12.0f, rcy + 28.0f}, m_pFontMono, COL_TEXT_PRIMARY);
            rcy += 44;
        }

        // 2. Головна кнопка запуску/зупинки віртуальної камери
        m_rcBtnStartStop = {rlx, rcy, rrx, rcy + 42.0f};
        bool isStr = m_state.isStreaming;
        DrawRoundedCard(
            m_rcBtnStartStop,
            isStr ? HexColor(0x3B1515) : HexColor(0x0E3A28),
            isStr ? COL_RED : COL_EMERALD,
            8.0f
        );
        std::wstring startStopLabel = isStr
            ? L"⏹ ЗУПИНИТИ ТРАНСЛЯЦІЮ"
            : L"▶ ЗАПУСТИТИ ВІРТУАЛЬНУ КАМЕРУ (DIRECTSHOW)";
        DrawTextStr(
            startStopLabel.c_str(),
            m_rcBtnStartStop,
            m_pFontSection,
            isStr ? HexColor(0xFCA5A5) : HexColor(0xA7F3D0),
            DWRITE_TEXT_ALIGNMENT_CENTER
        );
        rcy += 54;

        // 3. Оптика та Зум (0.5x..10x адаптивно)
        wchar_t lensTitle[80]{};
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

        float ispModeW = (rrx - rlx - 8.0f) * 0.58f;
        m_rcBtnModeToggle = {rlx, rcy, rlx + ispModeW, rcy + 32.0f};
        m_rcBtnRefocus    = {rlx + ispModeW + 8.0f, rcy, rrx, rcy + 32.0f};

        DrawRoundedCard(m_rcBtnModeToggle, m_state.isProManual ? HexColor(0x162C3D) : COL_BG_CARD,
                        m_state.isProManual ? COL_CYAN : COL_EMERALD, 8.0f);
        DrawTextStr(
            m_state.isProManual ? L"⚙ Режим: MANUAL" : L"✨ Режим: AUTO",
            m_rcBtnModeToggle, m_pFontBody, m_state.isProManual ? COL_CYAN : COL_EMERALD,
            DWRITE_TEXT_ALIGNMENT_CENTER
        );

        DrawRoundedCard(m_rcBtnRefocus, HexColor(0x182638), COL_CYAN, 8.0f);
        DrawTextStr(L"🎯 Автофокус", m_rcBtnRefocus, m_pFontBody, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
        rcy += 40;

        int isoVal = 100 + static_cast<int>(m_state.isoNorm * 3100.0f);
        int shutterDenom = 30 + static_cast<int>(m_state.shutterNorm * 3970.0f);
        int wbKelvin = 2800 + static_cast<int>(m_state.wbNorm * 5700.0f);

        wchar_t ispLabels[4][64]{};
        swprintf_s(ispLabels[0], L"ISO Чутливість: %d", isoVal);
        swprintf_s(ispLabels[1], L"Витримка (Shutter): 1/%d с", shutterDenom);
        swprintf_s(ispLabels[2], L"Фокус (Manual Focus): %.1f D", m_state.focusNorm * 10.0f);
        swprintf_s(ispLabels[3], L"Баланс білого: %d K", wbKelvin);

        float norms[4] = {m_state.isoNorm, m_state.shutterNorm, m_state.focusNorm, m_state.wbNorm};

        for (int s = 0; s < 4; ++s) {
            DrawTextStr(ispLabels[s], {rlx, rcy, rrx, rcy + 16}, m_pFontBody,
                        m_state.isProManual ? COL_TEXT_PRIMARY : COL_TEXT_DIM);
            rcy += 18;

            m_rcSliders[s] = {rlx, rcy, rrx, rcy + 12};
            UiRect track{rlx, rcy + 4, rrx, rcy + 8};
            DrawRoundedCard(track, COL_BG_DARK, COL_BORDER, 2.0f);

            if (m_state.isProManual) {
                float fillW = track.Width() * norms[s];
                DrawRoundedCard({rlx, rcy + 4, rlx + fillW, rcy + 8}, COL_CYAN, COL_CYAN, 2.0f);
                m_pBrush->SetColor(COL_TEXT_PRIMARY);
                m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rlx + fillW, rcy + 6.0f), 5.5f, 5.5f), m_pBrush);
            }
            rcy += 20;
        }
    }

    void DrawUsbDropdownOverlay() {
        std::lock_guard<std::mutex> lock(m_usbMutex);
        if (m_state.usbDevices.empty()) return;

        float topY = m_rcUsbDropdown.bottom + 4.0f;
        float itemH = 34.0f;
        float totalH = static_cast<float>(m_state.usbDevices.size()) * itemH;

        UiRect rcDropCard{m_rcUsbDropdown.left, topY, m_rcUsbDropdown.right, topY + totalH};
        DrawRoundedCard(rcDropCard, HexColor(0x181D26), COL_CYAN, 6.0f);

        m_rcUsbItems.resize(m_state.usbDevices.size());
        for (size_t i = 0; i < m_state.usbDevices.size(); ++i) {
            float iy = topY + i * itemH;
            m_rcUsbItems[i] = {rcDropCard.left, iy, rcDropCard.right, iy + itemH};

            bool sel = (m_state.selectedUsbIdx == static_cast<int>(i));
            if (sel) {
                DrawRoundedCard(m_rcUsbItems[i], HexColor(0x232C3A), HexColor(0x232C3A), 4.0f);
            }
            const auto& dev = m_state.usbDevices[i];
            std::wstring dStr = Utf8ToWide(dev.model) + L"  [" + Utf8ToWide(dev.serial) + L"]";
            DrawTextStr(dStr.c_str(), {m_rcUsbItems[i].left + 12.0f, iy + 8.0f, m_rcUsbItems[i].right - 12.0f, iy + 26.0f},
                        m_pFontBody, sel ? COL_CYAN : COL_TEXT_PRIMARY);
        }
    }

    void DrawRoundedCard(const UiRect& r, D2D1_COLOR_F fill, D2D1_COLOR_F border, float radius = 8.0f) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(r.left, r.top, r.right, r.bottom), radius, radius);
        m_pBrush->SetColor(fill);
        m_pRenderTarget->FillRoundedRectangle(rr, m_pBrush);
        m_pBrush->SetColor(border);
        m_pRenderTarget->DrawRoundedRectangle(rr, m_pBrush, 1.0f);
    }

    void DrawTextStr(const wchar_t* text, const UiRect& r, IDWriteTextFormat* format, D2D1_COLOR_F color,
                     DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!text || !format) return;
        format->SetTextAlignment(align);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_pBrush->SetColor(color);
        m_pRenderTarget->DrawText(
            text, static_cast<UINT32>(wcslen(text)), format,
            D2D1::RectF(r.left, r.top, r.right, r.bottom),
            m_pBrush
        );
    }

    // ========================================================================
    // Обробка взаємодії мишею (кліки, перетягування повзунків)
    // ========================================================================
    void OnLButtonDown(float x, float y) {
        // Якщо розгорнутий випадний список USB — перевіряємо вибір елемента
        if (m_state.usbDropdownOpen && m_state.transport == TransportTab::USB_ADB) {
            for (size_t i = 0; i < m_rcUsbItems.size(); ++i) {
                if (m_rcUsbItems[i].Contains(x, y)) {
                    m_state.selectedUsbIdx = static_cast<int>(i);
                    m_state.usbDropdownOpen = false;
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                    return;
                }
            }
            m_state.usbDropdownOpen = false;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return;
        }

        // Перемикач вкладок USB ADB / Wi-Fi
        if (m_rcTransportTabs[0].Contains(x, y)) {
            m_state.transport = TransportTab::USB_ADB;
            m_state.ipInputFocused = false;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return;
        }
        if (m_rcTransportTabs[1].Contains(x, y)) {
            m_state.transport = TransportTab::WIFI_LAN;
            m_state.ipInputFocused = false;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return;
        }

        // Розгортання списку USB
        if (m_state.transport == TransportTab::USB_ADB && m_rcUsbDropdown.Contains(x, y)) {
            m_state.usbDropdownOpen = !m_state.usbDropdownOpen;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return;
        }

        // Кнопка оновлення USB
        if (m_state.transport == TransportTab::USB_ADB && m_rcBtnRefreshUsb.Contains(x, y)) {
            RefreshUsbDeviceListAsync();
            return;
        }

        // Фокус на поле вводу IP (для Wi-Fi)
        if (m_state.transport == TransportTab::WIFI_LAN && m_rcIpInput.Contains(x, y)) {
            m_state.ipInputFocused = true;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return;
        } else {
            m_state.ipInputFocused = false;
        }

        // Кнопка реєстрації DirectShow драйвера
        if (m_rcBtnRegisterDriver.Contains(x, y)) {
            RegisterDirectShowDriver();
            m_state.isDirectShowRegistered = CheckDirectShowDriverRegistered();
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return;
        }

        // Кнопка Дзеркало
        if (m_rcBtnMirror.Contains(x, y)) {
            m_state.mirrorHorizontal = !m_state.mirrorHorizontal;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return;
        }

        // Кнопка Сітка
        if (m_rcBtnGrid.Contains(x, y)) {
            m_state.showFramingGrid = !m_state.showFramingGrid;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return;
        }

        // Головна кнопка START / STOP
        if (m_rcBtnStartStop.Contains(x, y)) {
            ToggleStreaming();
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return;
        }

        // Кнопки вибору об'єктивів (0.5x, 1x, 2x, 3x, 5x, 10x, Front)
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
                InvalidateRect(m_hWnd, nullptr, FALSE);
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

        // Кнопка примусового автофокусування
        if (m_rcBtnRefocus.Contains(x, y)) {
            SendControlJson("{\"action\":\"TriggerAutoFocus\"}");
            return;
        }

        // Повзунки ручного режиму (ISO, Shutter, Focus, WB)
        if (m_state.isProManual) {
            for (int s = 0; s < 4; ++s) {
                if (m_rcSliders[s].Contains(x, y)) {
                    m_activeSliderDrag = s;
                    UpdateSliderValue(s, x);
                    return;
                }
            }
        }
    }

    void OnMouseMove(float x, float y) {
        if (m_activeSliderDrag == 100) {
            UpdateZoomSliderValue(x);
        } else if (m_activeSliderDrag >= 0 && m_activeSliderDrag < 4) {
            UpdateSliderValue(m_activeSliderDrag, x);
        }
    }

    void OnLButtonUp() {
        m_activeSliderDrag = -1;
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

        if (idx == 0) {
            m_state.isoNorm = norm;
            int isoVal = 100 + static_cast<int>(norm * 3100.0f);
            char json[96]{};
            std::snprintf(json, sizeof(json), "{\"action\":\"SetISO\",\"auto\":false,\"iso\":%d}", isoVal);
            SendControlJson(json);
        } else if (idx == 1) {
            m_state.shutterNorm = norm;
            int denom = 30 + static_cast<int>(norm * 3970.0f);
            uint64_t ns = 1000000000ULL / denom;
            char json[128]{};
            std::snprintf(json, sizeof(json), "{\"action\":\"SetShutter\",\"auto\":false,\"exposure_ns\":%llu}", ns);
            SendControlJson(json);
        } else if (idx == 2) {
            m_state.focusNorm = norm;
            float diopters = norm * 10.0f;
            char json[96]{};
            std::snprintf(json, sizeof(json), "{\"action\":\"SetFocus\",\"mode\":\"MANUAL\",\"distance_diopters\":%.2f}", diopters);
            SendControlJson(json);
        } else if (idx == 3) {
            m_state.wbNorm = norm;
            int kelvin = 2800 + static_cast<int>(norm * 5700.0f);
            char json[96]{};
            std::snprintf(json, sizeof(json), "{\"action\":\"SetWhiteBalance\",\"mode\":\"MANUAL_KELVIN\",\"kelvin\":%d}", kelvin);
            SendControlJson(json);
        }
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }

    void OnChar(wchar_t ch) {
        if (!m_state.ipInputFocused || m_state.transport != TransportTab::WIFI_LAN) return;

        if (ch == VK_BACK) {
            if (!m_state.deviceIp.empty()) {
                m_state.deviceIp.pop_back();
            }
        } else if ((ch >= '0' && ch <= '9') || ch == '.') {
            if (m_state.deviceIp.size() < 15) {
                m_state.deviceIp.push_back(static_cast<char>(ch));
            }
        }
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }

    void ToggleStreaming() {
        if (m_state.isStreaming) {
            m_state.isStreaming = false;
            CloseActivePhoneSocket();
            if (m_sink.IsOpen()) {
                m_sink.Close();
            }
        } else {
            // Якщо використовується USB — гарантуємо прокидання порту ADB
            if (m_state.transport == TransportTab::USB_ADB) {
                std::string serial;
                {
                    std::lock_guard<std::mutex> lock(m_usbMutex);
                    if (m_state.selectedUsbIdx >= 0 && m_state.selectedUsbIdx < static_cast<int>(m_state.usbDevices.size())) {
                        serial = m_state.usbDevices[m_state.selectedUsbIdx].serial;
                    }
                }
                ForwardAdbPort(serial, 8554);
            }

            // Відкриваємо системний DirectShow sink віртуальної камери
            if (!m_sink.IsOpen()) {
                m_sink.Open(L"EyeCam Virtual Camera");
                m_sink.SetFormat(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2, m_state.fps);
            }
            m_state.isStreaming = true;
        }
    }

    static LRESULT CALLBACK WndProcThunk(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        EyeCamStudioWindow* pThis = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            pThis = reinterpret_cast<EyeCamStudioWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        } else {
            pThis = reinterpret_cast<EyeCamStudioWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        }

        if (pThis) {
            return pThis->HandleMessage(msg, wParam, lParam);
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(m_hWnd, &ps);
            Render();
            EndPaint(m_hWnd, &ps);
            return 0;
        }
        case WM_TIMER:
            m_frameTick++;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            SetCapture(m_hWnd);
            OnLButtonDown(static_cast<float>(LOWORD(lParam)), static_cast<float>(HIWORD(lParam)));
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove(static_cast<float>(LOWORD(lParam)), static_cast<float>(HIWORD(lParam)));
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            OnLButtonUp();
            return 0;
        case WM_CHAR:
            OnChar(static_cast<wchar_t>(wParam));
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(m_hWnd, msg, wParam, lParam);
    }
};

// ============================================================================
// Головна функція WinMain
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    EyeCamStudioWindow app;
    if (!app.Initialize(hInstance, nCmdShow)) {
        MessageBoxW(nullptr, L"Помилка ініціалізації вікна EyeCam Studio (Direct2D/WIC)", L"EyeCam Error", MB_ICONERROR);
        return 1;
    }
    return app.RunMessageLoop();
}
