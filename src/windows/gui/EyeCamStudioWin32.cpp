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
#include <ole2.h>
#include <dwmapi.h>
#include <shellapi.h>

#ifdef DrawText
#undef DrawText
#endif

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dxgiformat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WindowsDirectShowSink.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

#define IDI_EYECAM_APP 101

namespace {

// Палітра Windows 11 Dark Mode (Fluent 2 Design)
const D2D1_COLOR_F COL_BG_DARK        = D2D1::ColorF(0.051f, 0.055f, 0.063f, 1.0f); // #0D0E10
const D2D1_COLOR_F COL_PANEL_BG       = D2D1::ColorF(0.082f, 0.090f, 0.102f, 1.0f); // #15171A
const D2D1_COLOR_F COL_CARD_BG        = D2D1::ColorF(0.114f, 0.125f, 0.141f, 1.0f); // #1D2024
const D2D1_COLOR_F COL_CARD_HOVER     = D2D1::ColorF(0.145f, 0.160f, 0.180f, 1.0f); // #25292E
const D2D1_COLOR_F COL_CARD_ACTIVE    = D2D1::ColorF(0.180f, 0.200f, 0.225f, 1.0f); // #2E3339
const D2D1_COLOR_F COL_BORDER         = D2D1::ColorF(0.200f, 0.220f, 0.245f, 1.0f); // #33383E
const D2D1_COLOR_F COL_TEXT_PRIMARY   = D2D1::ColorF(0.960f, 0.965f, 0.970f, 1.0f); // #F5F6F7
const D2D1_COLOR_F COL_TEXT_SECOND    = D2D1::ColorF(0.650f, 0.670f, 0.700f, 1.0f); // #A6ABB3
const D2D1_COLOR_F COL_TEXT_DIM       = D2D1::ColorF(0.420f, 0.440f, 0.470f, 1.0f); // #6B7078
const D2D1_COLOR_F COL_CYAN           = D2D1::ColorF(0.000f, 0.898f, 1.000f, 1.0f); // #00E5FF
const D2D1_COLOR_F COL_EMERALD        = D2D1::ColorF(0.000f, 0.902f, 0.463f, 1.0f); // #00E676
const D2D1_COLOR_F COL_RED            = D2D1::ColorF(1.000f, 0.320f, 0.320f, 1.0f); // #FF5252

enum class TransportTab {
    USB_ADB = 0,
    WIFI_LAN = 1
};

struct UsbDeviceItem {
    std::string serial;
    std::string model;
    std::wstring displayName;
};

struct AppStudioState {
    TransportTab transport = TransportTab::USB_ADB;
    std::string deviceIp = "192.168.1.100";
    std::string selectedAdbSerial = "";
    std::vector<UsbDeviceItem> usbDevices;
    bool usbDropdownOpen = false;

    bool isStreaming = false;
    bool isPhoneConnected = false;
    bool isDriverRegistered = true;
    std::wstring connectedPhoneName = L"EyeCam Phone";

    // Налаштування роздільної здатності та кадру
    int resPresetIdx = 2; // 0=4K UHD, 1=2K QHD, 2=1080p, 3=720p
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;

    // Зум та оптика
    int activeLensIdx = 1; // 0=0.5x, 1=1x, 2=2x, 3=3x, 4=5x, 5=10x, 6=Front
    float currentZoom = 1.0f;
    float phoneMinZoom = 0.5f;
    float phoneMaxZoom = 10.0f;
    float zoomSliderNorm = 0.05f;

    // Сенсори / ISP
    int iso = 0;              // 0 = Auto
    double shutterSpeed = 0.0;// 0 = Auto
    float manualFocus = 0.0f; // 0 = Auto AF
    int whiteBalanceK = 0;    // 0 = Auto AWB
    bool mirrorHorizontal = false;
    bool showGrid = false;

    // Телеметрія потоку
    uint32_t measuredBitrateKbps = 0;
    uint32_t measuredFps = 0;
    uint64_t rxBytes = 0;
    uint64_t rxFrames = 0;
    uint64_t shmFramesWritten = 0;
};

struct UiRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
    bool Contains(float x, float y) const {
        return (x >= left && x <= right && y >= top && y <= bottom);
    }
};

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int count = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()), nullptr, 0);
    std::wstring wstr(count, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()), &wstr[0], count);
    return wstr;
}

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int count = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()), nullptr, 0, nullptr, nullptr);
    std::string str(count, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()), &str[0], count, nullptr, nullptr);
    return str;
}

bool ExtractJsonString(const std::string& json, const std::string& key, std::string& outVal) {
    std::string needle = "\"" + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.length();
    size_t end = json.find("\"", pos);
    if (end == std::string::npos) return false;
    outVal = json.substr(pos, end - pos);
    return true;
}

bool ExtractJsonFloat(const std::string& json, const std::string& key, float& outVal) {
    std::string needle = "\"" + key + "\":";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.length();
    char* endPtr = nullptr;
    outVal = std::strtof(json.c_str() + pos, &endPtr);
    return (endPtr != json.c_str() + pos);
}

// Пошук виконуваного файлу adb.exe
std::wstring FindAdbBinary() {
    WCHAR selfPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    std::wstring dir = selfPath;
    size_t lastSlash = dir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        dir = dir.substr(0, lastSlash);
    }

    std::wstring localAdb = dir + L"\\adb\\adb.exe";
    if (GetFileAttributesW(localAdb.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return localAdb;
    }
    localAdb = dir + L"\\adb.exe";
    if (GetFileAttributesW(localAdb.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return localAdb;
    }

    WCHAR envHome[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ANDROID_HOME", envHome, MAX_PATH) > 0) {
        std::wstring homeAdb = std::wstring(envHome) + L"\\platform-tools\\adb.exe";
        if (GetFileAttributesW(homeAdb.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return homeAdb;
        }
    }

    return L"adb.exe";
}

std::string RunCommandCapture(const std::wstring& cmd) {
    std::string result;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "";
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;

    PROCESS_INFORMATION pi{};
    std::wstring cmdMutable = cmd;

    if (CreateProcessW(nullptr, &cmdMutable[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWrite);
        hWrite = nullptr;

        char buf[512]{};
        DWORD bytesRead = 0;
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buf[bytesRead] = '\0';
            result += buf;
        }

        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        CloseHandle(hWrite);
    }

    CloseHandle(hRead);
    return result;
}

std::vector<UsbDeviceItem> DiscoverUsbDevices() {
    std::vector<UsbDeviceItem> list;
    std::wstring adb = FindAdbBinary();
    std::wstring cmd = L"\"" + adb + L"\" devices -l";

    std::string out = RunCommandCapture(cmd);
    std::istringstream stream(out);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.find("List of devices attached") != std::string::npos || line.empty()) continue;
        std::istringstream liness(line);
        std::string serial, state;
        liness >> serial >> state;
        if (state == "device") {
            UsbDeviceItem item;
            item.serial = serial;

            std::string token;
            while (liness >> token) {
                if (token.rfind("model:", 0) == 0) {
                    item.model = token.substr(6);
                }
            }
            if (item.model.empty()) item.model = "Android Device";

            for (char& c : item.model) {
                if (c == '_') c = ' ';
            }

            item.displayName = Utf8ToWide(item.model) + L" [" + Utf8ToWide(item.serial) + L"]";
            list.push_back(item);
        }
    }

    return list;
}

bool ForwardAdbPort(const std::string& serial) {
    std::wstring adb = FindAdbBinary();
    std::wstring cmd = L"\"" + adb + L"\"";
    if (!serial.empty()) {
        cmd += L" -s " + Utf8ToWide(serial);
    }
    cmd += L" forward tcp:8554 tcp:8554";
    std::string out = RunCommandCapture(cmd);
    return (out.find("error") == std::string::npos);
}

bool RegisterDirectShowDriver(HWND hwnd) {
    WCHAR selfPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    std::wstring dir = selfPath;
    size_t lastSlash = dir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        dir = dir.substr(0, lastSlash);
    }

    std::wstring dllPath = dir + L"\\EyeCamFilter.dll";
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        dllPath = dir + L"\\..\\lib\\EyeCamFilter.dll";
    }

    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hwnd, L"Файл драйвера камери EyeCamFilter.dll не знайдено поруч з додатком.",
                    L"EyeCam Studio - Драйвер", MB_OK | MB_ICONWARNING);
        return false;
    }

    HINSTANCE hDll = LoadLibraryW(dllPath.c_str());
    if (hDll) {
        typedef HRESULT(STDAPICALLTYPE* DllRegServerFunc)();
        DllRegServerFunc pfn = (DllRegServerFunc)GetProcAddress(hDll, "DllRegisterServer");
        if (pfn && SUCCEEDED(pfn())) {
            FreeLibrary(hDll);
            MessageBoxW(hwnd, L"Віртуальну камеру EyeCam успішно зареєстровано в системі!\nВона доступна у Zoom, Teams, Chrome та OBS.",
                        L"EyeCam Studio - Успіх", MB_OK | MB_ICONINFORMATION);
            return true;
        }
        FreeLibrary(hDll);
    }

    // Якщо прав недостатньо — виклик regsvr32 через UAC
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = L"regsvr32.exe";
    std::wstring params = L"/s \"" + dllPath + L"\"";
    sei.lpParameters = params.c_str();
    sei.hwnd = hwnd;
    sei.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei)) {
        MessageBoxW(hwnd, L"Команду реєстрації віртуальної камери виконано з правами адміністратора.",
                    L"EyeCam Studio", MB_OK | MB_ICONINFORMATION);
        return true;
    }

    return false;
}

} // anonymous namespace

class EyeCamStudioApp {
public:
    EyeCamStudioApp() = default;
    ~EyeCamStudioApp() {
        StopStreaming();
        CleanupD2D();
    }

    bool Initialize(HINSTANCE hInstance, int nCmdShow) {
        m_hInstance = hInstance;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = StaticWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_EYECAM_APP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_EYECAM_APP), IMAGE_ICON, 16, 16, LR_SHARED);
        wc.lpszClassName = L"EyeCamStudioMainWindowClass";

        RegisterClassExW(&wc);

        m_hwnd = CreateWindowExW(
            WS_EX_APPWINDOW,
            wc.lpszClassName,
            L"EyeCam Studio Pro — 4K UHD DirectShow Camera Engine",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
            nullptr, nullptr, hInstance, this
        );

        if (!m_hwnd) return false;

        // Підтримка Windows 11 Dark Mode Title Bar
        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));

        InitD2D();
        RefreshUsbDevices();

        ShowWindow(m_hwnd, nCmdShow);
        UpdateWindow(m_hwnd);

        // Таймер перерисовки UI та телеметрії (60 FPS)
        SetTimer(m_hwnd, 1, 16, nullptr);
        SetTimer(m_hwnd, 2, 2000, nullptr); // Оновлення USB кожні 2 секунди

        return true;
    }

    int Run() {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        EyeCamStudioApp* app = nullptr;
        if (uMsg == WM_NCCREATE) {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = reinterpret_cast<EyeCamStudioApp*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->m_hwnd = hwnd;
        } else {
            app = reinterpret_cast<EyeCamStudioApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (app) {
            return app->HandleMessage(uMsg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
        switch (uMsg) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(m_hwnd, &ps);
                Render();
                EndPaint(m_hwnd, &ps);
                return 0;
            }
            case WM_SIZE: {
                if (m_pRenderTarget) {
                    RECT rc;
                    GetClientRect(m_hwnd, &rc);
                    m_pRenderTarget->Resize(D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
                }
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;
            }
            case WM_TIMER: {
                if (wParam == 1) {
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                } else if (wParam == 2) {
                    RefreshUsbDevices();
                }
                return 0;
            }
            case WM_LBUTTONDOWN: {
                float x = static_cast<float>(GET_X_LPARAM(lParam));
                float y = static_cast<float>(GET_Y_LPARAM(lParam));
                OnLButtonDown(x, y);
                return 0;
            }
            case WM_MOUSEMOVE: {
                float x = static_cast<float>(GET_X_LPARAM(lParam));
                float y = static_cast<float>(GET_Y_LPARAM(lParam));
                OnMouseMove(x, y, wParam);
                return 0;
            }
            case WM_LBUTTONUP: {
                OnLButtonUp();
                return 0;
            }
            case WM_CHAR: {
                OnChar(static_cast<wchar_t>(wParam));
                return 0;
            }
            case WM_DESTROY: {
                KillTimer(m_hwnd, 1);
                KillTimer(m_hwnd, 2);
                PostQuitMessage(0);
                return 0;
            }
        }
        return DefWindowProcW(m_hwnd, uMsg, wParam, lParam);
    }

    void RefreshUsbDevices() {
        if (m_state.usbDropdownOpen) return;
        auto devs = DiscoverUsbDevices();
        if (devs != m_state.usbDevices) {
            m_state.usbDevices = devs;
            if (!m_state.usbDevices.empty()) {
                if (m_state.selectedAdbSerial.empty()) {
                    m_state.selectedAdbSerial = m_state.usbDevices[0].serial;
                }
            } else {
                m_state.selectedAdbSerial = "";
            }
        }
    }

    void StartStreaming() {
        if (m_state.isStreaming) return;
        m_state.isStreaming = true;
        m_state.rxBytes = 0;
        m_state.rxFrames = 0;

        if (m_state.transport == TransportTab::USB_ADB) {
            ForwardAdbPort(m_state.selectedAdbSerial);
        }

        m_sink.Open(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2, m_state.fps);

        m_running = true;
        m_clientThread = std::thread(&EyeCamStudioApp::PhoneStreamClientLoop, this);
    }

    void StopStreaming() {
        m_running = false;
        m_state.isStreaming = false;
        m_state.isPhoneConnected = false;

        {
            std::lock_guard<std::mutex> lock(m_socketMutex);
            if (m_phoneSocket != INVALID_SOCKET) {
                closesocket(m_phoneSocket);
                m_phoneSocket = INVALID_SOCKET;
            }
        }

        if (m_clientThread.joinable()) {
            m_clientThread.join();
        }

        m_sink.Close();
    }

    void SendControlJson(const std::string& jsonPayload) {
        std::thread([this, jsonPayload]() {
            uint16_t magic = 0x4350; // 'CP'
            uint16_t msgType = 1;     // MSG_TYPE_JSON_COMMAND
            uint32_t len = static_cast<uint32_t>(jsonPayload.size());

            uint8_t header[8]{};
            header[0] = static_cast<uint8_t>((magic >> 8) & 0xFF);
            header[1] = static_cast<uint8_t>(magic & 0xFF);
            header[2] = static_cast<uint8_t>((msgType >> 8) & 0xFF);
            header[3] = static_cast<uint8_t>(msgType & 0xFF);
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
            int nodelay = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
            int rcvBufSize = 4 * 1024 * 1024; // 4MB буфер для надплавної передачі 60 FPS без затримок
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBufSize), sizeof(rcvBufSize));

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

            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                if (m_phoneSocket == sock) {
                    m_phoneSocket = INVALID_SOCKET;
                }
            }
            closesocket(sock);
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

        // Швидка векторна / вказівникова конвертація BGRA -> YUY2 без ділення на піксель
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

    void DrawTextStr(const std::wstring& text, const UiRect& r, IDWriteTextFormat* fmt, const D2D1_COLOR_F& color,
                     DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!m_pRenderTarget || !m_pBrush || !fmt) return;
        m_pBrush->SetColor(color);
        fmt->SetTextAlignment(align);
        D2D1_RECT_F d2r = D2D1::RectF(r.left, r.top, r.right, r.bottom);
        m_pRenderTarget->DrawText(
            text.c_str(),
            static_cast<UINT32>(text.length()),
            fmt,
            &d2r,
            m_pBrush
        );
    }

    void DrawRoundedCard(const UiRect& r, const D2D1_COLOR_F& fillCol, const D2D1_COLOR_F& borderCol, float radius = 8.0f) {
        if (!m_pRenderTarget || !m_pBrush) return;
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(r.left, r.top, r.right, r.bottom), radius, radius);
        m_pBrush->SetColor(fillCol);
        m_pRenderTarget->FillRoundedRectangle(&rr, m_pBrush);
        m_pBrush->SetColor(borderCol);
        m_pRenderTarget->DrawRoundedRectangle(&rr, m_pBrush, 1.0f);
    }

    void Render() {
        EnsureRenderTarget();
        if (!m_pRenderTarget) return;

        m_pRenderTarget->BeginDraw();
        m_pRenderTarget->Clear(COL_BG_DARK);

        RECT clientRc{};
        GetClientRect(m_hwnd, &clientRc);
        float winW = static_cast<float>(clientRc.right - clientRc.left);
        float winH = static_cast<float>(clientRc.bottom - clientRc.top);

        const float pad = 16.0f;
        const float topBarH = 46.0f;
        const float colLeftW = 320.0f;
        const float colRightW = 340.0f;

        // --- ВЕРХНІЙ ЗАГОЛОВОК СТУДІЇ ---
        DrawRoundedCard({pad, pad, winW - pad, pad + topBarH}, COL_PANEL_BG, COL_BORDER, 10.0f);

        // Малювання стилізованого логотипу EyeCam у шапці
        D2D1_ROUNDED_RECT logoR = D2D1::RoundedRect(D2D1::RectF(pad + 12, pad + 8, pad + 44, pad + 38), 6.0f, 6.0f);
        m_pBrush->SetColor(COL_CARD_ACTIVE);
        m_pRenderTarget->FillRoundedRectangle(&logoR, m_pBrush);
        m_pBrush->SetColor(COL_CYAN);
        m_pRenderTarget->DrawRoundedRectangle(&logoR, m_pBrush, 1.5f);
        D2D1_ELLIPSE pupil = D2D1::Ellipse(D2D1::Point2F(pad + 28, pad + 23), 6.0f, 6.0f);
        m_pRenderTarget->FillEllipse(&pupil, m_pBrush);

        DrawTextStr(L"EYECAM STUDIO", {pad + 56, pad + 6, pad + 250, pad + topBarH}, m_pFontTitle, COL_TEXT_PRIMARY);
        DrawTextStr(L"4K UHD DirectShow & ADB/Wi-Fi Pro Camera Engine", {pad + 245, pad + 14, pad + 680, pad + topBarH}, m_pFontBody, COL_TEXT_SECOND);

        // Статус бейдж драйвера DirectShow
        UiRect badgeDriver{winW - pad - 230, pad + 10, winW - pad - 12, pad + 36};
        DrawRoundedCard(badgeDriver, m_state.isDriverRegistered ? D2D1::ColorF(0.0f, 0.3f, 0.15f, 0.4f) : D2D1::ColorF(0.3f, 0.1f, 0.1f, 0.4f),
                        m_state.isDriverRegistered ? COL_EMERALD : COL_RED, 6.0f);
        DrawTextStr(
            m_state.isDriverRegistered ? L"✔ DirectShow Драйвер Активний" : L"⚠ Драйвер не зареєстровано",
            badgeDriver, m_pFontBody, m_state.isDriverRegistered ? COL_EMERALD : COL_RED, DWRITE_TEXT_ALIGNMENT_CENTER
        );

        float bodyTop = pad + topBarH + 12.0f;
        float bodyH = winH - bodyTop - pad;

        // =====================================================================
        // КОЛОНКА 1: ПІДКЛЮЧЕННЯ (USB ADB та WI-FI LAN)
        // =====================================================================
        UiRect colLeft{pad, bodyTop, pad + colLeftW, bodyTop + bodyH};
        DrawRoundedCard(colLeft, COL_PANEL_BG, COL_BORDER, 10.0f);

        float lx = colLeft.left + 16.0f;
        float rx = colLeft.right - 16.0f;
        float cy = colLeft.top + 16.0f;

        DrawTextStr(L"1. СПОСІБ ПІДКЛЮЧЕННЯ", {lx, cy, rx, cy + 24}, m_pFontSection, COL_TEXT_PRIMARY);
        cy += 28.0f;

        // Вкладки USB ADB vs Wi-Fi LAN
        float tabW = (rx - lx - 8.0f) / 2.0f;
        m_rcTransportTabs[0] = {lx, cy, lx + tabW, cy + 34.0f};
        m_rcTransportTabs[1] = {lx + tabW + 8.0f, cy, rx, cy + 34.0f};

        const wchar_t* tabNames[2] = {L"⚡ USB (ADB)", L"📶 Wi-Fi LAN"};
        for (int i = 0; i < 2; ++i) {
            bool sel = (static_cast<int>(m_state.transport) == i);
            DrawRoundedCard(m_rcTransportTabs[i], sel ? COL_CARD_ACTIVE : COL_CARD_BG, sel ? COL_CYAN : COL_BORDER, 6.0f);
            DrawTextStr(tabNames[i], m_rcTransportTabs[i], m_pFontSection,
                        sel ? COL_CYAN : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        cy += 44.0f;

        if (m_state.transport == TransportTab::WIFI_LAN) {
            DrawTextStr(L"IP-адреса смартфона (див. у додатку):", {lx, cy, rx, cy + 20}, m_pFontBody, COL_TEXT_SECOND);
            cy += 22.0f;

            m_rcIpInput = {lx, cy, rx, cy + 38.0f};
            DrawRoundedCard(m_rcIpInput, COL_CARD_BG, COL_BORDER, 6.0f);
            std::wstring ipW = Utf8ToWide(m_state.deviceIp);
            if (m_editingIp) ipW += L"|";
            DrawTextStr(L"  " + ipW, m_rcIpInput, m_pFontMono, COL_TEXT_PRIMARY);
            cy += 46.0f;

            DrawTextStr(L"Порт синхронізації та відео (TCP): 8554", {lx, cy, rx, cy + 20}, m_pFontBody, COL_TEXT_DIM);
            cy += 30.0f;
        } else {
            // USB ADB: Список підключених пристроїв
            DrawTextStr(L"Виберіть смартфон, підключений по USB:", {lx, cy, rx, cy + 20}, m_pFontBody, COL_TEXT_SECOND);
            cy += 22.0f;

            m_rcUsbDropdown = {lx, cy, rx, cy + 38.0f};
            DrawRoundedCard(m_rcUsbDropdown, COL_CARD_BG, m_state.usbDropdownOpen ? COL_CYAN : COL_BORDER, 6.0f);

            std::wstring selectedUsbLabel = L"  Пошук підключеного смартфона...";
            for (const auto& dev : m_state.usbDevices) {
                if (dev.serial == m_state.selectedAdbSerial) {
                    selectedUsbLabel = L"  " + dev.displayName;
                    break;
                }
            }
            if (m_state.usbDevices.empty()) {
                selectedUsbLabel = L"  (Немає підключених USB-пристроїв)";
            }

            DrawTextStr(selectedUsbLabel, {lx, cy, rx - 28, cy + 38}, m_pFontBody,
                        m_state.usbDevices.empty() ? COL_TEXT_DIM : COL_TEXT_PRIMARY);
            DrawTextStr(m_state.usbDropdownOpen ? L"▲" : L"▼", {rx - 28, cy, rx - 6, cy + 38}, m_pFontBody, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
            cy += 46.0f;

            // Кнопка примусового оновлення списку ADB
            m_rcBtnRefreshUsb = {lx, cy, rx, cy + 32.0f};
            DrawRoundedCard(m_rcBtnRefreshUsb, COL_CARD_BG, COL_BORDER, 6.0f);
            DrawTextStr(L"↻  Оновити список USB-пристроїв", m_rcBtnRefreshUsb, m_pFontBody, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
            cy += 42.0f;
        }

        // Кнопка СТАРТ / СТОП
        m_rcBtnStreamToggle = {lx, cy, rx, cy + 44.0f};
        D2D1_COLOR_F btnCol = m_state.isStreaming ? COL_RED : COL_EMERALD;
        DrawRoundedCard(m_rcBtnStreamToggle, m_state.isStreaming ? D2D1::ColorF(0.35f, 0.1f, 0.1f) : D2D1::ColorF(0.0f, 0.35f, 0.18f),
                        btnCol, 8.0f);
        DrawTextStr(
            m_state.isStreaming ? L"■  ЗУПИНИТИ ТРАНСЛЯЦІЮ" : L"▶  ПОЧАТИ ТРАНСЛЯЦІЮ",
            m_rcBtnStreamToggle, m_pFontSection, COL_TEXT_PRIMARY, DWRITE_TEXT_ALIGNMENT_CENTER
        );
        cy += 58.0f;

        // Блок керування системним драйвером камери
        DrawTextStr(L"2. СИСТЕМНИЙ ДРАЙВЕР КАМЕРИ", {lx, cy, rx, cy + 24}, m_pFontSection, COL_TEXT_PRIMARY);
        cy += 28.0f;

        UiRect driverCard{lx, cy, rx, cy + 74.0f};
        DrawRoundedCard(driverCard, COL_CARD_BG, COL_BORDER, 8.0f);
        DrawTextStr(L"Камера: EyeCam Virtual Camera", {lx + 12, cy + 8, rx - 12, cy + 30}, m_pFontBody, COL_TEXT_PRIMARY);
        DrawTextStr(
            L"Пряма трансляція в Zoom, Teams, Chrome",
            {lx + 12, cy + 28, rx - 12, cy + 48}, m_pFontBody, COL_TEXT_SECOND
        );
        DrawTextStr(
            m_state.isDriverRegistered ? L"Статус: Зареєстровано" : L"Статус: Потрібна реєстрація",
            {lx + 12, cy + 48, rx - 12, cy + 68}, m_pFontBody,
            m_state.isDriverRegistered ? COL_EMERALD : COL_RED
        );
        cy += 84.0f;

        m_rcBtnRegisterDriver = {lx, cy, rx, cy + 34.0f};
        DrawRoundedCard(m_rcBtnRegisterDriver, COL_CARD_BG, COL_BORDER, 6.0f);
        DrawTextStr(
            m_state.isDriverRegistered ? L"↻  Перереєструвати віртуальну камеру" : L"★  Зареєструвати віртуальну камеру",
            m_rcBtnRegisterDriver, m_pFontBody, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER
        );

        // Випадаюче меню USB ADB пристроїв поверх інтерфейсу
        if (m_state.usbDropdownOpen) {
            float dropTop = m_rcUsbDropdown.bottom + 2.0f;
            float dropH = (std::max)(1u, static_cast<uint32_t>(m_state.usbDevices.size())) * 32.0f + 8.0f;
            UiRect dropBox{m_rcUsbDropdown.left, dropTop, m_rcUsbDropdown.right, dropTop + dropH};
            DrawRoundedCard(dropBox, COL_CARD_ACTIVE, COL_CYAN, 6.0f);

            if (m_state.usbDevices.empty()) {
                DrawTextStr(L"  Немає активних USB ADB пристроїв", dropBox, m_pFontBody, COL_TEXT_SECOND);
            } else {
                for (size_t i = 0; i < m_state.usbDevices.size(); ++i) {
                    float itemY = dropTop + 4.0f + i * 32.0f;
                    UiRect itemR{dropBox.left, itemY, dropBox.right, itemY + 30.0f};
                    bool hovered = itemR.Contains(m_lastMouseX, m_lastMouseY);
                    if (hovered) {
                        DrawRoundedCard(itemR, COL_CARD_HOVER, COL_CYAN, 4.0f);
                    }
                    DrawTextStr(L"  " + m_state.usbDevices[i].displayName, itemR, m_pFontBody, COL_TEXT_PRIMARY);
                }
            }
        }

        // =====================================================================
        // КОЛОНКА 2: ЦЕНТРАЛЬНИЙ МОНІТОР ПЕРЕГЛЯДУ (FULL HD / 4K HUD)
        // =====================================================================
        float midLeft = pad + colLeftW + 12.0f;
        float midRight = winW - pad - colRightW - 12.0f;
        UiRect colCenter{midLeft, bodyTop, midRight, bodyTop + bodyH};
        DrawRoundedCard(colCenter, COL_PANEL_BG, COL_BORDER, 10.0f);

        // Розрахунок прямокутника відеотрансляції 16:9
        float maxVidW = colCenter.Width() - 32.0f;
        float maxVidH = colCenter.Height() - 100.0f;
        float vidW = maxVidW;
        float vidH = vidW * 9.0f / 16.0f;
        if (vidH > maxVidH) {
            vidH = maxVidH;
            vidW = vidH * 16.0f / 9.0f;
        }

        float vidX = colCenter.left + (colCenter.Width() - vidW) / 2.0f;
        float vidY = colCenter.top + 20.0f + (maxVidH - vidH) / 2.0f;
        UiRect videoRect{vidX, vidY, vidX + vidW, vidY + vidH};

        // Чорна рама прев'ю екрана
        DrawRoundedCard(videoRect, D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), COL_BORDER, 8.0f);

        // Відображення живого кадру з камери смартфона (якщо отримано через WIC)
        bool renderedLiveFrame = false;
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            if (m_hasLiveFrame && !m_previewPixels.empty() && m_pPreviewBitmap) {
                D2D1_RECT_U destRect = D2D1::RectU(0, 0, 640, 360);
                m_pPreviewBitmap->CopyFromMemory(&destRect, m_previewPixels.data(), 640 * 4);
                D2D1_RECT_F renderDest = D2D1::RectF(videoRect.left, videoRect.top, videoRect.right, videoRect.bottom);
                m_pRenderTarget->DrawBitmap(m_pPreviewBitmap, &renderDest);
                renderedLiveFrame = true;
            }
        }

        if (!renderedLiveFrame) {
            float midY = videoRect.top + videoRect.Height() / 2.0f;
            if (!m_state.isStreaming) {
                DrawTextStr(L"ТРАНСЛЯЦІЮ ПРИЗУПИНЕНО", {videoRect.left, midY - 12, videoRect.right, midY + 18},
                            m_pFontSection, COL_TEXT_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (m_state.transport == TransportTab::USB_ADB) {
                DrawTextStr(L"ОЧІКУВАННЯ ВІДЕОПОТОКУ ПО USB (ADB)...", {videoRect.left, midY - 14, videoRect.right, midY + 14},
                            m_pFontSection, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
                DrawTextStr(
                    L"Підключіть телефон кабелем і натисніть Play у додатку EyeCam",
                    {videoRect.left, midY + 14, videoRect.right, midY + 36}, m_pFontBody, COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER
                );
            } else {
                std::wstring waitWifi = L"ОЧІКУВАННЯ ПІДКЛЮЧЕННЯ ДО " + Utf8ToWide(m_state.deviceIp) + L":8554...";
                DrawTextStr(waitWifi, {videoRect.left, midY - 14, videoRect.right, midY + 14},
                            m_pFontSection, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
                DrawTextStr(
                    L"Переконайтеся, що комп'ютер і телефон підключені до однієї Wi-Fi мережі",
                    {videoRect.left, midY + 14, videoRect.right, midY + 36}, m_pFontBody, COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER
                );
            }
        }

        // Сітка 3x3 для правила третин
        if (m_state.showGrid) {
            m_pBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.2f));
            float x1 = videoRect.left + videoRect.Width() / 3.0f;
            float x2 = videoRect.left + 2.0f * videoRect.Width() / 3.0f;
            m_pRenderTarget->DrawLine(D2D1::Point2F(x1, videoRect.top), D2D1::Point2F(x1, videoRect.bottom), m_pBrush, 1.0f);
            m_pRenderTarget->DrawLine(D2D1::Point2F(x2, videoRect.top), D2D1::Point2F(x2, videoRect.bottom), m_pBrush, 1.0f);

            float y1 = videoRect.top + videoRect.Height() / 3.0f;
            float y2 = videoRect.top + 2.0f * videoRect.Height() / 3.0f;
            m_pRenderTarget->DrawLine(D2D1::Point2F(videoRect.left, y1), D2D1::Point2F(videoRect.right, y1), m_pBrush, 1.0f);
            m_pRenderTarget->DrawLine(D2D1::Point2F(videoRect.left, y2), D2D1::Point2F(videoRect.right, y2), m_pBrush, 1.0f);
        }

        // Плаваючі інформаційні плашки на відео (HUD)
        if (m_state.isStreaming) {
            UiRect hudBadge1{videoRect.left + 12.0f, videoRect.top + 12.0f, videoRect.left + 130.0f, videoRect.top + 36.0f};
            DrawRoundedCard(hudBadge1, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.7f), COL_BORDER, 6.0f);

            wchar_t badgeBuf[64]{};
            swprintf_s(badgeBuf, L"● %ls", m_state.isPhoneConnected ? L"LIVE STREAM" : L"CONNECTING");
            DrawTextStr(badgeBuf, hudBadge1, m_pFontMono, m_state.isPhoneConnected ? COL_EMERALD : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);

            UiRect hudBadge2{videoRect.right - 130.0f, videoRect.top + 12.0f, videoRect.right - 12.0f, videoRect.top + 36.0f};
            DrawRoundedCard(hudBadge2, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.7f), COL_BORDER, 6.0f);
            wchar_t resFpsBuf[64]{};
            swprintf_s(resFpsBuf, L"%ux%u @ %ufps", m_state.width, m_state.height, m_state.fps);
            DrawTextStr(resFpsBuf, hudBadge2, m_pFontMono, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // Нижній блок телеметрії (біткрейт, fps, shm frames)
        UiRect teleBar{colCenter.left + 16.0f, colCenter.bottom - 44.0f, colCenter.right - 16.0f, colCenter.bottom - 12.0f};
        DrawRoundedCard(teleBar, COL_CARD_BG, COL_BORDER, 6.0f);

        wchar_t teleStr[256]{};
        swprintf_s(teleStr, L"   Телеметрія:  Бітрейт: %u кбіт/с   |   Швидкість: %u FPS   |   Кадрів у DirectShow SHM: %llu   |   Пристрій: %ls",
                   m_state.measuredBitrateKbps, m_state.measuredFps, m_state.shmFramesWritten, m_state.connectedPhoneName.c_str());
        DrawTextStr(teleStr, teleBar, m_pFontMono, COL_TEXT_SECOND);

        // =====================================================================
        // КОЛОНКА 3: ОПТИКА, ЗУМ ТА ISP ПАРАМЕТРИ КАМЕРИ
        // =====================================================================
        UiRect colRight{winW - pad - colRightW, bodyTop, winW - pad, bodyTop + bodyH};
        DrawRoundedCard(colRight, COL_PANEL_BG, COL_BORDER, 10.0f);

        float rlx = colRight.left + 16.0f;
        float rrx = colRight.right - 16.0f;
        float rcy = colRight.top + 16.0f;

        // Блок 1: Оптика та пресети зуму
        wchar_t lensTitle[64]{};
        swprintf_s(lensTitle, L"3. ОПТИКА ТА ЗУМ (МАКС: %.0fx)", m_state.phoneMaxZoom);
        DrawTextStr(lensTitle, {rlx, rcy, rrx, rcy + 22}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 26.0f;

        const wchar_t* lensLabels[7] = {L"0.5x", L"1x", L"2x", L"3x", L"5x", L"10x", L"Front"};
        const float lensZooms[7] = {0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f, 1.0f};
        float btnGap = 4.0f;
        float lensW = (rrx - rlx - 6.0f * btnGap) / 7.0f;

        for (int i = 0; i < 7; ++i) {
            m_rcLensBtns[i] = {rlx + i * (lensW + btnGap), rcy, rlx + i * (lensW + btnGap) + lensW, rcy + 32.0f};
            bool sel = (m_state.activeLensIdx == i);
            bool supported = true;
            if (i == 0 && m_state.phoneMinZoom > 0.7f) supported = false;
            if (i >= 1 && i <= 5 && lensZooms[i] > m_state.phoneMaxZoom + 0.05f) supported = false;

            D2D1_COLOR_F fillCol = sel ? COL_CARD_ACTIVE : COL_CARD_BG;
            D2D1_COLOR_F brdCol = sel ? COL_CYAN : (supported ? COL_BORDER : D2D1::ColorF(0.15f, 0.16f, 0.18f));
            D2D1_COLOR_F txtCol = sel ? COL_CYAN : (supported ? COL_TEXT_PRIMARY : COL_TEXT_DIM);

            DrawRoundedCard(m_rcLensBtns[i], fillCol, brdCol, 7.0f);
            DrawTextStr(lensLabels[i], m_rcLensBtns[i], m_pFontBody, txtCol, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        rcy += 40.0f;

        // Повзунок плавного зуму (від phoneMinZoom до phoneMaxZoom, напр. 0.5x .. 10.0x)
        wchar_t zoomSliderText[64]{};
        swprintf_s(zoomSliderText, L"Плавний зум: %.1fx (Діапазон: %.1fx .. %.1fx)",
                   m_state.currentZoom, m_state.phoneMinZoom, m_state.phoneMaxZoom);
        DrawTextStr(zoomSliderText, {rlx, rcy, rrx, rcy + 18}, m_pFontBody, COL_CYAN);
        rcy += 20.0f;

        m_rcZoomSlider = {rlx, rcy, rrx, rcy + 20.0f};
        DrawSliderControl(m_rcZoomSlider, m_state.zoomSliderNorm, COL_CYAN);
        rcy += 30.0f;

        // Блок 2: Роздільна здатність (4K, 2K, 1080p, 720p)
        DrawTextStr(L"4. РОЗДІЛЬНА ЗДАТНІСТЬ (ДО 4K UHD)", {rlx, rcy, rrx, rcy + 22}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 26.0f;

        const wchar_t* resLabels[4] = {L"4K UHD", L"2K QHD", L"1080p 60", L"720p 60"};
        float resW = (rrx - rlx - 3.0f * btnGap) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            m_rcResBtns[i] = {rlx + i * (resW + btnGap), rcy, rlx + i * (resW + btnGap) + resW, rcy + 32.0f};
            bool sel = (m_state.resPresetIdx == i);
            DrawRoundedCard(m_rcResBtns[i], sel ? COL_CARD_ACTIVE : COL_CARD_BG, sel ? COL_CYAN : COL_BORDER, 6.0f);
            DrawTextStr(resLabels[i], m_rcResBtns[i], m_pFontBody,
                        sel ? COL_CYAN : COL_TEXT_PRIMARY, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        rcy += 42.0f;

        // Блок 3: Експозиція, фокус та колір (ISP)
        DrawTextStr(L"5. ЕКСПОЗИЦІЯ ТА КОЛІР (ISP)", {rlx, rcy, rrx, rcy + 22}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 26.0f;

        // Кнопка незалежного автофокусу / перефокусування
        m_rcBtnRefocus = {rlx, rcy, rrx, rcy + 32.0f};
        DrawRoundedCard(m_rcBtnRefocus, COL_CARD_BG, COL_BORDER, 6.0f);
        DrawTextStr(
            L"🎯 Автофокус", m_rcBtnRefocus, m_pFontBody, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER
        );
        rcy += 38.0f;

        const wchar_t* sliderTitles[4] = {
            L"ISO Чутливість (Auto / Manual)",
            L"Витримка затвора (1/s)",
            L"Ручний фокус (діоптрії)",
            L"Баланс білого (Kelvin)"
        };

        for (int i = 0; i < 4; ++i) {
            DrawTextStr(sliderTitles[i], {rlx, rcy, rrx, rcy + 16}, m_pFontBody,
                        m_sliderActive[i] ? COL_CYAN : COL_TEXT_SECOND);
            rcy += 18.0f;

            m_rcSliders[i] = {rlx, rcy, rrx, rcy + 18.0f};
            DrawSliderControl(m_rcSliders[i], m_sliderNorm[i], COL_CYAN);
            rcy += 26.0f;
        }

        // Додаткові швидкі перемикачі (Дзеркало, Сітка)
        float toggleW = (rrx - rlx - 8.0f) / 2.0f;
        m_rcBtnMirror = {rlx, rcy, rlx + toggleW, rcy + 32.0f};
        m_rcBtnGrid = {rlx + toggleW + 8.0f, rcy, rrx, rcy + 32.0f};

        DrawRoundedCard(m_rcBtnMirror, m_state.mirrorHorizontal ? COL_CARD_ACTIVE : COL_CARD_BG,
                        m_state.mirrorHorizontal ? COL_CYAN : COL_BORDER, 6.0f);
        DrawTextStr(L"⇄ Дзеркало", m_rcBtnMirror, m_pFontBody,
                    m_state.mirrorHorizontal ? COL_CYAN : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);

        DrawRoundedCard(m_rcBtnGrid, m_state.showGrid ? COL_CARD_ACTIVE : COL_CARD_BG,
                        m_state.showGrid ? COL_CYAN : COL_BORDER, 6.0f);
        DrawTextStr(L"# Сітка 3x3", m_rcBtnGrid, m_pFontBody,
                    m_state.showGrid ? COL_CYAN : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);

        m_pRenderTarget->EndDraw();
    }

    void DrawSliderControl(const UiRect& r, float normValue, const D2D1_COLOR_F& activeColor) {
        if (!m_pRenderTarget || !m_pBrush) return;

        float trackY = r.top + r.Height() / 2.0f;
        D2D1_ROUNDED_RECT track = D2D1::RoundedRect(D2D1::RectF(r.left, trackY - 2.5f, r.right, trackY + 2.5f), 2.5f, 2.5f);
        m_pBrush->SetColor(COL_BORDER);
        m_pRenderTarget->FillRoundedRectangle(&track, m_pBrush);

        float fillX = r.left + normValue * r.Width();
        D2D1_ROUNDED_RECT filled = D2D1::RoundedRect(D2D1::RectF(r.left, trackY - 2.5f, fillX, trackY + 2.5f), 2.5f, 2.5f);
        m_pBrush->SetColor(activeColor);
        m_pRenderTarget->FillRoundedRectangle(&filled, m_pBrush);

        D2D1_ELLIPSE thumb = D2D1::Ellipse(D2D1::Point2F(fillX, trackY), 7.0f, 7.0f);
        m_pBrush->SetColor(COL_TEXT_PRIMARY);
        m_pRenderTarget->FillEllipse(&thumb, m_pBrush);
        m_pBrush->SetColor(activeColor);
        m_pRenderTarget->DrawEllipse(&thumb, m_pBrush, 2.0f);
    }

    void OnLButtonDown(float x, float y) {
        SetCapture(m_hwnd);

        // Якщо відкрите випадаюче меню USB ADB
        if (m_state.usbDropdownOpen) {
            float dropTop = m_rcUsbDropdown.bottom + 2.0f;
            for (size_t i = 0; i < m_state.usbDevices.size(); ++i) {
                float itemY = dropTop + 4.0f + i * 32.0f;
                UiRect itemR{m_rcUsbDropdown.left, itemY, m_rcUsbDropdown.right, itemY + 30.0f};
                if (itemR.Contains(x, y)) {
                    m_state.selectedAdbSerial = m_state.usbDevices[i].serial;
                    m_state.usbDropdownOpen = false;
                    return;
                }
            }
            m_state.usbDropdownOpen = false;
            return;
        }

        // Перемикання вкладок підключення
        for (int i = 0; i < 2; ++i) {
            if (m_rcTransportTabs[i].Contains(x, y)) {
                m_state.transport = static_cast<TransportTab>(i);
                return;
            }
        }

        // Відкриття списку USB
        if (m_state.transport == TransportTab::USB_ADB && m_rcUsbDropdown.Contains(x, y)) {
            m_state.usbDropdownOpen = !m_state.usbDropdownOpen;
            return;
        }

        // Кнопка оновлення USB
        if (m_state.transport == TransportTab::USB_ADB && m_rcBtnRefreshUsb.Contains(x, y)) {
            RefreshUsbDevices();
            return;
        }

        // Поле вводу IP-адреси
        if (m_state.transport == TransportTab::WIFI_LAN && m_rcIpInput.Contains(x, y)) {
            m_editingIp = true;
            return;
        } else {
            m_editingIp = false;
        }

        // Кнопка СТАРТ / СТОП
        if (m_rcBtnStreamToggle.Contains(x, y)) {
            if (m_state.isStreaming) {
                StopStreaming();
            } else {
                StartStreaming();
            }
            return;
        }

        // Кнопка реєстрації драйвера DirectShow
        if (m_rcBtnRegisterDriver.Contains(x, y)) {
            m_state.isDriverRegistered = RegisterDirectShowDriver(m_hwnd);
            return;
        }

        // Вибір пресету зуму / об'єктива (0.5x, 1x, 2x, 3x, 5x, 10x, Front)
        const char* lensNames[7] = {"0.5x", "1.0x", "2.0x", "3.0x", "5.0x", "10.0x", "front"};
        const float lensZooms[7] = {0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f, 1.0f};
        for (int i = 0; i < 7; ++i) {
            if (m_rcLensBtns[i].Contains(x, y)) {
                if (i == 0 && m_state.phoneMinZoom > 0.7f) return;
                if (i >= 1 && i <= 5 && lensZooms[i] > m_state.phoneMaxZoom + 0.05f) return;

                m_state.activeLensIdx = i;
                m_state.currentZoom = lensZooms[i];
                float span = (std::max)(0.1f, m_state.phoneMaxZoom - m_state.phoneMinZoom);
                m_state.zoomSliderNorm = std::clamp((m_state.currentZoom - m_state.phoneMinZoom) / span, 0.0f, 1.0f);

                char json[128]{};
                std::snprintf(json, sizeof(json), "{\"action\":\"SetCameraModule\",\"module\":\"%s\",\"zoom\":%.2f}", lensNames[i], lensZooms[i]);
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
                    m_sink.Open(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2, m_state.fps);
                }

                char json[160]{};
                std::snprintf(json, sizeof(json), "{\"action\":\"SetResolution\",\"width\":%u,\"height\":%u,\"fps\":%u}",
                              m_state.width, m_state.height, m_state.fps);
                SendControlJson(json);
                return;
            }
        }

        // Кнопка незалежного автофокусу / перефокусування
        if (m_rcBtnRefocus.Contains(x, y)) {
            SendControlJson("{\"action\":\"Refocus\"}");
            return;
        }

        // Повзунки ISP (ISO, Shutter, Focus, WB)
        for (int i = 0; i < 4; ++i) {
            if (m_rcSliders[i].Contains(x, y)) {
                m_activeSliderDrag = i;
                UpdateSliderValue(i, x);
                return;
            }
        }

        // Кнопка дзеркала
        if (m_rcBtnMirror.Contains(x, y)) {
            m_state.mirrorHorizontal = !m_state.mirrorHorizontal;
            return;
        }

        // Кнопка сітки
        if (m_rcBtnGrid.Contains(x, y)) {
            m_state.showGrid = !m_state.showGrid;
            return;
        }
    }

    void OnMouseMove(float x, float y, WPARAM wParam) {
        m_lastMouseX = x;
        m_lastMouseY = y;

        if (wParam & MK_LBUTTON) {
            if (m_activeSliderDrag == 100) {
                UpdateZoomSliderValue(x);
            } else if (m_activeSliderDrag >= 0 && m_activeSliderDrag < 4) {
                UpdateSliderValue(m_activeSliderDrag, x);
            }
        }
    }

    void OnLButtonUp() {
        ReleaseCapture();
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

        char json[160]{};
        if (idx == 0) {
            // ISO: 0 (Auto) .. 3200
            m_sliderNorm[0] = norm;
            m_sliderActive[0] = (norm > 0.05f);
            m_state.iso = (norm <= 0.05f) ? 0 : static_cast<int>(50 + norm * 3150);
            std::snprintf(json, sizeof(json), "{\"action\":\"SetISO\",\"iso\":%d}", m_state.iso);
        } else if (idx == 1) {
            // Shutter: 0 (Auto) .. 1/1000s
            m_sliderNorm[1] = norm;
            m_sliderActive[1] = (norm > 0.05f);
            m_state.shutterSpeed = (norm <= 0.05f) ? 0.0 : (10.0 + norm * 990.0);
            std::snprintf(json, sizeof(json), "{\"action\":\"SetShutter\",\"shutter\":%.1f}", m_state.shutterSpeed);
        } else if (idx == 2) {
            // Focus: 0 (Auto AF) .. 10.0 diopters
            m_sliderNorm[2] = norm;
            m_sliderActive[2] = (norm > 0.05f);
            m_state.manualFocus = (norm <= 0.05f) ? 0.0f : (norm * 10.0f);
            std::snprintf(json, sizeof(json), "{\"action\":\"SetFocus\",\"focus\":%.2f}", m_state.manualFocus);
        } else if (idx == 3) {
            // White Balance: 0 (Auto AWB) .. 2000K .. 9000K
            m_sliderNorm[3] = norm;
            m_sliderActive[3] = (norm > 0.05f);
            m_state.whiteBalanceK = (norm <= 0.05f) ? 0 : static_cast<int>(2000 + norm * 7000);
            std::snprintf(json, sizeof(json), "{\"action\":\"SetWB\",\"kelvin\":%d}", m_state.whiteBalanceK);
        }

        SendControlJson(json);
    }

    void OnChar(wchar_t c) {
        if (!m_editingIp) return;
        if (c == VK_BACK) {
            if (!m_state.deviceIp.empty()) {
                m_state.deviceIp.pop_back();
            }
        } else if (c >= 32 && c <= 126) {
            if (m_state.deviceIp.length() < 32) {
                m_state.deviceIp.push_back(static_cast<char>(c));
            }
        }
    }

    void InitD2D() {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pD2DFactory);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&m_pDWriteFactory));

        if (m_pDWriteFactory) {
            m_pDWriteFactory->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 17.0f, L"en-US", &m_pFontTitle
            );
            m_pDWriteFactory->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-US", &m_pFontSection
            );
            m_pDWriteFactory->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-US", &m_pFontBody
            );
            m_pDWriteFactory->CreateTextFormat(
                L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-US", &m_pFontMono
            );
        }
    }

    void CleanupD2D() {
        if (m_pPreviewBitmap) { m_pPreviewBitmap->Release(); m_pPreviewBitmap = nullptr; }
        if (m_pBrush) { m_pBrush->Release(); m_pBrush = nullptr; }
        if (m_pRenderTarget) { m_pRenderTarget->Release(); m_pRenderTarget = nullptr; }
        if (m_pFontTitle) { m_pFontTitle->Release(); m_pFontTitle = nullptr; }
        if (m_pFontSection) { m_pFontSection->Release(); m_pFontSection = nullptr; }
        if (m_pFontBody) { m_pFontBody->Release(); m_pFontBody = nullptr; }
        if (m_pFontMono) { m_pFontMono->Release(); m_pFontMono = nullptr; }
        if (m_pDWriteFactory) { m_pDWriteFactory->Release(); m_pDWriteFactory = nullptr; }
        if (m_pD2DFactory) { m_pD2DFactory->Release(); m_pD2DFactory = nullptr; }
    }

private:
    HINSTANCE m_hInstance = nullptr;
    HWND m_hwnd = nullptr;

    ID2D1Factory* m_pD2DFactory = nullptr;
    IDWriteFactory* m_pDWriteFactory = nullptr;
    ID2D1HwndRenderTarget* m_pRenderTarget = nullptr;
    ID2D1SolidColorBrush* m_pBrush = nullptr;
    ID2D1Bitmap* m_pPreviewBitmap = nullptr;

    IDWriteTextFormat* m_pFontTitle = nullptr;
    IDWriteTextFormat* m_pFontSection = nullptr;
    IDWriteTextFormat* m_pFontBody = nullptr;
    IDWriteTextFormat* m_pFontMono = nullptr;

    AppStudioState m_state;
    EyeCam::WindowsDirectShowSink m_sink;

    // Мережевий потік прийому живого відео від смартфона
    std::atomic<bool> m_running{false};
    std::thread m_clientThread;
    SOCKET m_phoneSocket = INVALID_SOCKET;
    std::mutex m_socketMutex;

    // Подвійна буферизація живого кадру (WIC -> D2D Preview Bitmap)
    std::mutex m_frameMutex;
    bool m_hasLiveFrame = false;
    std::vector<uint8_t> m_previewPixels;
    std::vector<uint8_t> m_cachedPreviewBuffer;
    std::vector<uint8_t> m_fullBgraBuffer;
    std::vector<uint8_t> m_yuy2Buffer;

    // UI стан та прямокутники кліків
    bool m_editingIp = false;
    int m_activeSliderDrag = -1; // -1 = немає, 100 = zoom, 0..3 = ISP
    float m_sliderNorm[4]{0.0f, 0.0f, 0.0f, 0.0f};
    bool m_sliderActive[4]{false, false, false, false};
    float m_lastMouseX = 0.0f;
    float m_lastMouseY = 0.0f;

    UiRect m_rcTransportTabs[2]{};
    UiRect m_rcIpInput{};
    UiRect m_rcUsbDropdown{};
    UiRect m_rcBtnRefreshUsb{};
    UiRect m_rcBtnStreamToggle{};
    UiRect m_rcBtnRegisterDriver{};
    UiRect m_rcLensBtns[7]{};
    UiRect m_rcZoomSlider{};
    UiRect m_rcResBtns[4]{};
    UiRect m_rcBtnRefocus{};
    UiRect m_rcSliders[4]{};
    UiRect m_rcBtnMirror{};
    UiRect m_rcBtnGrid{};
};

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    EyeCamStudioApp app;
    int res = 0;
    if (app.Initialize(hInstance, nCmdShow)) {
        res = app.Run();
    }

    WSACleanup();
    return res;
}

#endif // _WIN32
