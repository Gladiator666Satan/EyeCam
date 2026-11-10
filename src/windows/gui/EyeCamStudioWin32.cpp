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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WindowsDirectShowSink.hpp"
#include "SharedMemoryProtocol.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")

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
// Колірна гамма у стилі Windows 11 Fluent Dark (Mica / Dark Mode)
// ============================================================================
const D2D1_COLOR_F COL_BG_DARK          = D2D1::ColorF(0.06f, 0.06f, 0.08f, 1.0f); // #0F0F14
const D2D1_COLOR_F COL_CARD_BG          = D2D1::ColorF(0.11f, 0.11f, 0.14f, 1.0f); // #1C1C24
const D2D1_COLOR_F COL_CARD_BORDER      = D2D1::ColorF(0.18f, 0.18f, 0.22f, 1.0f); // #2E2E38
const D2D1_COLOR_F COL_CARD_ACTIVE_BRD  = D2D1::ColorF(0.24f, 0.24f, 0.32f, 1.0f);

const D2D1_COLOR_F COL_ACCENT_PRIMARY   = D2D1::ColorF(0.00f, 0.85f, 0.65f, 1.0f); // #00D9A5
const D2D1_COLOR_F COL_ACCENT_CYAN      = D2D1::ColorF(0.00f, 0.80f, 0.95f, 1.0f); // #00CCF2
const D2D1_COLOR_F COL_ACCENT_DANGER    = D2D1::ColorF(0.95f, 0.26f, 0.21f, 1.0f); // #F24336
const D2D1_COLOR_F COL_ACCENT_HOVER     = D2D1::ColorF(0.15f, 0.15f, 0.20f, 1.0f);

const D2D1_COLOR_F COL_TEXT_PRIMARY     = D2D1::ColorF(0.96f, 0.96f, 0.97f, 1.0f); // #F5F5F7
const D2D1_COLOR_F COL_TEXT_SECONDARY   = D2D1::ColorF(0.65f, 0.65f, 0.70f, 1.0f); // #A6A6B3
const D2D1_COLOR_F COL_TEXT_MUTED       = D2D1::ColorF(0.40f, 0.40f, 0.46f, 1.0f); // #666675

// ============================================================================
// Допоміжні структури
// ============================================================================
struct UiRect {
    float left = 0, top = 0, right = 0, bottom = 0;
    bool Contains(float x, float y) const {
        return x >= left && x <= right && y >= top && y <= bottom;
    }
    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
    D2D1_RECT_F ToD2D() const { return D2D1::RectF(left, top, right, bottom); }
};

enum class TransportTab {
    USB_ADB = 0,
    WIFI_LAN = 1
};

struct UsbDeviceItem {
    std::string serial;
    std::string model;
};

struct StudioState {
    TransportTab transport = TransportTab::USB_ADB;
    std::string phoneIp = "192.168.1.100";
    int phonePort = 8554;
    std::string adbDeviceSerial = "";
    std::vector<UsbDeviceItem> usbDevices;
    int selectedUsbDeviceIndex = 0;
    bool usbDropdownOpen = false;

    bool isStreaming = false;
    bool isDirectShowActive = false;
    bool isFilterRegistered = false;

    // Телеметрія потоку
    uint32_t measuredFps = 0;
    uint32_t measuredBitrateKbps = 0;
    uint32_t rxFrames = 0;
    uint64_t shmFramesWritten = 0;
    float currentRttMs = 2.4f;

    // Дані смартфона
    std::string phoneModelName = "Samsung Galaxy S26 Ultra";
    float phoneMinZoom = 0.5f;
    float phoneMaxZoom = 10.0f;
    bool phoneSupports4K = true;
    std::vector<float> phoneZoomPresets = {0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f};

    // Роздільна здатність (0: 4K UHD, 1: 2K QHD, 2: 1080p, 3: 720p)
    int resPresetIdx = 2; // 1080p за замовчуванням
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;

    // Керування оптикою та зумом (0.5x, 1x, 2x, 3x, 5x, 10x, Front)
    int activeLensIdx = 1; // 0: 0.5x, 1: 1x, 2: 2x, 3: 3x, 4: 5x, 5: 10x, 6: Front
    float currentZoom = 1.0f;
    float zoomSliderNorm = 0.05f;

    // Професійний ручний режим (ISP Pro Manual)
    bool isProManual = false;
    float isoNorm = 0.15f;        // 100 .. 3200
    float shutterNorm = 0.25f;    // 1/30s .. 1/4000s
    float focusNorm = 0.0f;       // Auto .. Manual Infinity
    float wbNorm = 0.50f;         // 2800K .. 8500K

    // Ефекти та компонування
    bool mirrorHorizontal = false;
    bool showGrid = false;
};

// ============================================================================
// Перевірка та реєстрація DirectShow DLL у реєстрі Windows
// ============================================================================
bool CheckDirectShowDriverRegistered() {
    HKEY hKey = nullptr;
    // Шукаємо CLSID нашого EyeCam Source Filter
    const wchar_t* subKey = L"CLSID\\{A786B6D1-0F98-4B89-8A19-9D8B4D49A001}";
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, subKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

std::wstring GetModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    return std::wstring(path);
}

// Завантаження фірмової іконки: спочатку з файлу "eyecam_pc.ico" у теці програми, потім з ресурсів
HICON LoadCustomStudioIcon(HINSTANCE hInstance) {
    std::wstring icoPath = GetModuleDirectory() + L"\\eyecam_pc.ico";
    HICON hIco = static_cast<HICON>(LoadImageW(
        nullptr,
        icoPath.c_str(),
        IMAGE_ICON,
        128, 128,
        LR_LOADFROMFILE | LR_SHARED
    ));
    if (!hIco) {
        hIco = static_cast<HICON>(LoadImageW(
            hInstance,
            MAKEINTRESOURCEW(101),
            IMAGE_ICON,
            128, 128,
            LR_SHARED
        ));
    }
    if (!hIco) {
        hIco = LoadIcon(nullptr, IDI_APPLICATION);
    }
    return hIco;
}

// Сканування доступних пристроїв через adb devices -l
std::vector<UsbDeviceItem> ScanAdbDevices() {
    std::vector<UsbDeviceItem> result;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe = nullptr;
    HANDLE hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return result;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;

    PROCESS_INFORMATION pi{};
    wchar_t cmd[] = L"adb.exe devices -l";

    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWritePipe);
        hWritePipe = nullptr;

        std::string output;
        char buffer[256];
        DWORD bytesRead = 0;
        while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            output.append(buffer, bytesRead);
        }
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        // Парсинг виводу adb devices
        size_t pos = 0;
        while (pos < output.size()) {
            size_t endLine = output.find('\n', pos);
            if (endLine == std::string::npos) endLine = output.size();
            std::string line = output.substr(pos, endLine - pos);
            pos = endLine + 1;

            if (line.find("List of devices") != std::string::npos || line.empty()) continue;

            size_t spacePos = line.find_first_of(" \t");
            if (spacePos != std::string::npos) {
                std::string serial = line.substr(0, spacePos);
                std::string rest = line.substr(spacePos);
                if (rest.find("device") != std::string::npos && rest.find("offline") == std::string::npos) {
                    UsbDeviceItem item;
                    item.serial = serial;
                    item.model = "Android Device (" + serial + ")";

                    size_t modelPos = rest.find("model:");
                    if (modelPos != std::string::npos) {
                        size_t modelEnd = rest.find_first_of(" \t\r\n", modelPos + 6);
                        std::string rawModel = rest.substr(modelPos + 6, (modelEnd != std::string::npos) ? (modelEnd - (modelPos + 6)) : std::string::npos);
                        for (auto& c : rawModel) { if (c == '_') c = ' '; }
                        item.model = rawModel + " [" + serial + "]";
                    }
                    result.push_back(item);
                }
            }
        }
    } else {
        if (hWritePipe) CloseHandle(hWritePipe);
    }

    if (hReadPipe) CloseHandle(hReadPipe);
    return result;
}

// Запуск adb forward tcp:8554 tcp:8554 для зв'язку через USB
bool ConfigureAdbPortForwarding(const std::string& specificSerial = "") {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::wstring cmd = L"adb.exe";
    if (!specificSerial.empty()) {
        std::wstring wsSerial(specificSerial.begin(), specificSerial.end());
        cmd += L" -s " + wsSerial;
    }
    cmd += L" forward tcp:8554 tcp:8554";

    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
    return false;
}

// Автоматична реєстрація DirectShow драйвера через regsvr32 з UAC правами
bool ElevateAndRegisterDirectShowDriver(HWND hwnd) {
    std::wstring dllPath = GetModuleDirectory() + L"\\EyeCamFilter.dll";
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Fallback: шукаємо у ../lib/ або build/lib/
        dllPath = GetModuleDirectory() + L"\\..\\lib\\EyeCamFilter.dll";
        if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            dllPath = GetModuleDirectory() + L"\\EyeCamFilter.dll";
        }
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

        // Завантаження фірмової іконки ноутбука з оком та бліком об'єктива (eyecam_pc.ico)
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
            L"EyeCam Studio — Ultra-Low Latency 4K Virtual Webcam",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 780,
            nullptr, nullptr, hInstance, this
        );
        if (!m_hwnd) return false;

        SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hAppIcon));
        SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hAppIcon));

        // Windows 11 Dark Mode, Mica-ефект та закруглені кути
        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
        DWORD cornerPref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
        COLORREF captionColor = RGB(15, 15, 20);
        DwmSetWindowAttribute(m_hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

        m_state.isFilterRegistered = CheckDirectShowDriverRegistered();

        // Первинне фонове сканування підключених USB телефонів
        RefreshUsbDevicesAsync();

        ShowWindow(m_hwnd, nCmdShow);
        UpdateWindow(m_hwnd);

        // Таймер перевірки статусу та оновлення USB списку
        SetTimer(m_hwnd, 1, 16, nullptr);  // ~60 FPS перемальовування
        SetTimer(m_hwnd, 2, 2500, nullptr); // періодичний чек adb devices

        return true;
    }

    void RunMessageLoop() {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void Shutdown() {
        StopStreaming();
        KillTimer(m_hwnd, 1);
        KillTimer(m_hwnd, 2);

        DiscardDeviceResources();
        if (m_pFormatRegular) { m_pFormatRegular->Release(); m_pFormatRegular = nullptr; }
        if (m_pFormatBold) { m_pFormatBold->Release(); m_pFormatBold = nullptr; }
        if (m_pFormatMonospace) { m_pFormatMonospace->Release(); m_pFormatMonospace = nullptr; }
        if (m_pFormatTitle) { m_pFormatTitle->Release(); m_pFormatTitle = nullptr; }
        if (m_pFormatBadge) { m_pFormatBadge->Release(); m_pFormatBadge = nullptr; }

        if (m_pDWriteFactory) { m_pDWriteFactory->Release(); m_pDWriteFactory = nullptr; }
        if (m_pD2DFactory) { m_pD2DFactory->Release(); m_pD2DFactory = nullptr; }

        CoUninitialize();
        WSACleanup();
    }

    void StartStreaming() {
        if (m_state.isStreaming) return;
        m_state.isStreaming = true;
        m_running = true;

        // Ініціалізація DirectShow IPC Virtual Camera Sink
        m_sink.Open(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2, m_state.fps);
        m_state.isDirectShowActive = m_sink.IsOpen();

        if (m_receiverThread.joinable()) m_receiverThread.join();

        m_receiverThread = std::thread([this]() {
            StreamReceiverLoop();
        });
    }

    void StopStreaming() {
        m_running = false;
        m_state.isStreaming = false;

        // Безпечне закриття сокета для негайного виходу з recv()
        {
            std::lock_guard<std::mutex> lock(m_socketMutex);
            if (m_phoneSocket != INVALID_SOCKET) {
                closesocket(m_phoneSocket);
                m_phoneSocket = INVALID_SOCKET;
            }
        }

        if (m_receiverThread.joinable()) {
            m_receiverThread.join();
        }

        m_sink.Close();
        m_state.isDirectShowActive = false;

        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            m_previewPixels.clear();
            m_hasLiveFrame = false;
        }

        m_state.rxFrames = 0;
        m_state.measuredFps = 0;
        m_state.measuredBitrateKbps = 0;
    }

    void RefreshUsbDevicesAsync() {
        std::thread([this]() {
            auto devs = ScanAdbDevices();
            PostTaskToUi([this, devs]() {
                m_state.usbDevices = devs;
                if (!devs.empty()) {
                    if (m_state.selectedUsbDeviceIndex >= static_cast<int>(devs.size())) {
                        m_state.selectedUsbDeviceIndex = 0;
                    }
                    m_state.adbDeviceSerial = devs[m_state.selectedUsbDeviceIndex].serial;
                } else {
                    m_state.adbDeviceSerial = "";
                }
            });
        }).detach();
    }

    void PostTaskToUi(std::function<void()> task) {
        auto* fnPtr = new std::function<void()>(task);
        PostMessageW(m_hwnd, WM_APP + 42, reinterpret_cast<WPARAM>(fnPtr), 0);
    }

private:
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
                send(m_phoneSocket, jsonPayload.data(), static_cast<int>(len), 0);
            }
        }).detach();
    }

    bool RecvExact(SOCKET s, uint8_t* buf, size_t len) {
        size_t total = 0;
        while (total < len && m_running) {
            int ret = recv(s, reinterpret_cast<char*>(buf + total), static_cast<int>(len - total), 0);
            if (ret <= 0) return false;
            total += ret;
        }
        return total == len;
    }

    void StreamReceiverLoop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        IWICImagingFactory* pWicFactory = nullptr;
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&pWicFactory)
        );

        while (m_running && m_state.isStreaming) {
            std::string connectHost = "127.0.0.1";
            int connectPort = 8554;

            if (m_state.transport == TransportTab::USB_ADB) {
                ConfigureAdbPortForwarding(m_state.adbDeviceSerial);
                connectHost = "127.0.0.1";
                connectPort = 8554;
            } else {
                connectHost = m_state.phoneIp;
                connectPort = m_state.phonePort;
            }

            SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            // Швидкі сокетні буфери для 60 FPS та відключення алгоритму Nagle (TCP_NODELAY)
            int flag = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&flag), sizeof(flag));
            int rcvBuf = 2 * 1024 * 1024;
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&rcvBuf), sizeof(rcvBuf));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<u_short>(connectPort));
            inet_pton(AF_INET, connectHost.c_str(), &addr.sin_addr);

            // Таймаут на з'єднання
            u_long nonBlocking = 1;
            ioctlsocket(sock, FIONBIO, &nonBlocking);
            connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(sock, &writeSet);
            timeval tv{ 1, 500000 };
            int sel = select(0, nullptr, &writeSet, nullptr, &tv);

            if (sel <= 0) {
                closesocket(sock);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            nonBlocking = 0;
            ioctlsocket(sock, FIONBIO, &nonBlocking);

            // Таймаут на прийом даних
            DWORD timeoutMs = 2500;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeoutMs), sizeof(timeoutMs));

            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                m_phoneSocket = sock;
            }

            // Запит можливостей та профілю оптики пристрою
            SendControlJson("{\"action\":\"GetCapabilities\"}");

            std::vector<uint8_t> payload;
            auto lastStatTime = std::chrono::steady_clock::now();
            uint64_t bytesWindow = 0;
            uint32_t framesWindow = 0;

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

                if (payload.size() < len) payload.resize(len);
                if (!RecvExact(sock, payload.data(), len)) break;

                bytesWindow += (8 + len);

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
                    closesocket(sock);
                    m_phoneSocket = INVALID_SOCKET;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        if (pWicFactory) pWicFactory->Release();
        CoUninitialize();
    }

    void ParseDeviceCapabilitiesJson(const std::string& json) {
        std::string model;
        if (ExtractJsonString(json, "model_name", model) || ExtractJsonString(json, "device_name", model)) {
            m_state.phoneModelName = model;
        }
        float minZ = 0.5f, maxZ = 10.0f;
        if (ExtractJsonFloat(json, "min_zoom", minZ)) m_state.phoneMinZoom = minZ;
        if (ExtractJsonFloat(json, "max_zoom", maxZ)) m_state.phoneMaxZoom = maxZ;
        if (json.find("\"supports_4k\":true") != std::string::npos) {
            m_state.phoneSupports4K = true;
        }
    }

    void DecodeIncomingJpegFrame(IWICImagingFactory* pWicFactory, const uint8_t* jpegData, size_t jpegSize) {
        if (!pWicFactory || !jpegData || jpegSize == 0) return;

        IWICStream* pStream = nullptr;
        HRESULT hr = pWicFactory->CreateStream(&pStream);
        if (FAILED(hr) || !pStream) return;

        hr = pStream->InitializeFromMemory(const_cast<BYTE*>(jpegData), static_cast<DWORD>(jpegSize));
        if (FAILED(hr)) {
            pStream->Release();
            return;
        }

        IWICBitmapDecoder* pDecoder = nullptr;
        hr = pWicFactory->CreateDecoderFromStream(
            pStream,
            nullptr,
            WICDecodeMetadataCacheOnDemand,
            &pDecoder
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

        // Повнорозмірний YUY2 вивід без масштабування у нативній роздільній здатності
        uint32_t outW = inW;
        uint32_t outH = inH;

        size_t yuy2Size = static_cast<size_t>(outW) * outH * 2;
        if (m_yuy2Buffer.size() != yuy2Size) {
            m_yuy2Buffer.resize(yuy2Size);
        }

        // Швидка векторна конвертація BGRA у YUY2 для DirectShow Virtual Camera
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

    void DrawRoundedCard(const UiRect& r, const D2D1_COLOR_F& fill, const D2D1_COLOR_F& border, float radius = 10.0f) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r.ToD2D(), radius, radius);
        m_pBrush->SetColor(fill);
        m_pRenderTarget->FillRoundedRectangle(rr, m_pBrush);
        m_pBrush->SetColor(border);
        m_pRenderTarget->DrawRoundedRectangle(rr, m_pBrush, 1.0f);
    }

    void RenderScene() {
        EnsureRenderTarget();
        if (!m_pRenderTarget) return;

        m_pRenderTarget->BeginDraw();
        m_pRenderTarget->Clear(COL_BG_DARK);

        RECT clientRc{};
        GetClientRect(m_hwnd, &clientRc);
        float width = static_cast<float>(clientRc.right);
        float height = static_cast<float>(clientRc.bottom);

        // Векторний логотип: стилізований контур ноутбука + око з бліком об'єктива
        DrawHeaderBar(width);

        // 3-колоночний макет Windows 11 Fluent Design
        float margin = 16.0f;
        float headerH = 68.0f;
        float bodyTop = headerH + margin;
        float bodyH = height - bodyTop - margin;

        float leftColW = 320.0f;
        float rightColW = 300.0f;
        float centerColW = width - leftColW - rightColW - (margin * 4.0f);

        UiRect leftPanel{ margin, bodyTop, margin + leftColW, bodyTop + bodyH };
        UiRect centerPanel{ leftPanel.right + margin, bodyTop, leftPanel.right + margin + centerColW, bodyTop + bodyH };
        UiRect rightPanel{ centerPanel.right + margin, bodyTop, centerPanel.right + margin + rightColW, bodyTop + bodyH };

        RenderLeftControlPanel(leftPanel);
        RenderCenterPreviewPanel(centerPanel);
        RenderRightOpticsPanel(rightPanel);

        if (m_state.usbDropdownOpen) {
            RenderUsbDropdownOverlay();
        }

        HRESULT hr = m_pRenderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            DiscardDeviceResources();
        }
    }

    void DrawHeaderBar(float width) {
        UiRect hdrRect{ 0, 0, width, 68.0f };
        m_pBrush->SetColor(COL_CARD_BG);
        m_pRenderTarget->FillRectangle(hdrRect.ToD2D(), m_pBrush);
        m_pBrush->SetColor(COL_CARD_BORDER);
        m_pRenderTarget->DrawLine(D2D1::Point2F(0, 68.0f), D2D1::Point2F(width, 68.0f), m_pBrush, 1.0f);

        // Малювання іконки Ноутбук + Око з бліком
        DrawLaptopEyeLogo(20.0f, 14.0f, 40.0f);

        // Заголовок та слоган
        m_pBrush->SetColor(COL_TEXT_PRIMARY);
        m_pRenderTarget->DrawText(
            L"EyeCam Studio", 13,
            m_pFormatTitle,
            D2D1::RectF(74.0f, 12.0f, 300.0f, 38.0f),
            m_pBrush
        );

        m_pBrush->SetColor(COL_TEXT_SECONDARY);
        m_pRenderTarget->DrawText(
            L"Ultra-Low Latency 4K Virtual Camera Engine", 41,
            m_pFormatRegular,
            D2D1::RectF(74.0f, 38.0f, 450.0f, 58.0f),
            m_pBrush
        );

        // Статус DirectShow драйвера у системі (Virtual Device Status)
        DrawDriverStatusBadge(width - 240.0f, 18.0f);
    }

    void DrawLaptopEyeLogo(float x, float y, float size) {
        float w = size * 1.1f;
        float h = size * 0.72f;

        // 1. Рамка ноутбука (Screen Lid)
        D2D1_ROUNDED_RECT lid = D2D1::RoundedRect(D2D1::RectF(x + 4.0f, y, x + w - 4.0f, y + h), 4.0f, 4.0f);
        m_pBrush->SetColor(COL_CARD_BORDER);
        m_pRenderTarget->DrawRoundedRectangle(lid, m_pBrush, 2.0f);

        // 2. База ноутбука (Laptop Keyboard Base)
        m_pRenderTarget->DrawLine(D2D1::Point2F(x, y + h + 4.0f), D2D1::Point2F(x + w, y + h + 4.0f), m_pBrush, 2.5f);
        m_pRenderTarget->DrawLine(D2D1::Point2F(x + w * 0.4f, y + h + 5.0f), D2D1::Point2F(x + w * 0.6f, y + h + 5.0f), m_pBrush, 3.0f);

        // 3. Стилізоване живе око в центрі екрана
        float cx = x + w * 0.5f;
        float cy = y + h * 0.5f;

        // Зовнішня райдужка (Eye Iris)
        m_pBrush->SetColor(COL_ACCENT_PRIMARY);
        m_pRenderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 9.0f, 6.5f), m_pBrush, 1.8f);

        // Зіниця (Pupil)
        m_pBrush->SetColor(COL_ACCENT_CYAN);
        m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 4.0f, 4.0f), m_pBrush);

        // Блік об'єктива (Lens Glare Highlight)
        m_pBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
        m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 2.0f, cy - 2.0f), 1.5f, 1.5f), m_pBrush);
    }

    void DrawDriverStatusBadge(float x, float y) {
        UiRect badge{ x, y, x + 220.0f, y + 32.0f };
        bool reg = m_state.isFilterRegistered;

        DrawRoundedCard(badge, reg ? D2D1::ColorF(0.0f, 0.85f, 0.65f, 0.12f) : D2D1::ColorF(0.95f, 0.26f, 0.21f, 0.12f),
                               reg ? COL_ACCENT_PRIMARY : COL_ACCENT_DANGER, 16.0f);

        // Маркер
        m_pBrush->SetColor(reg ? COL_ACCENT_PRIMARY : COL_ACCENT_DANGER);
        m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 14.0f, y + 16.0f), 4.5f, 4.5f), m_pBrush);

        const wchar_t* txt = reg ? L"DirectShow: Готовий (OBS/Zoom)" : L"DirectShow: Потрібна дія";
        m_pBrush->SetColor(COL_TEXT_PRIMARY);
        m_pRenderTarget->DrawText(
            txt, static_cast<UINT32>(wcslen(txt)),
            m_pFormatBadge,
            D2D1::RectF(x + 26.0f, y + 8.0f, x + 215.0f, y + 26.0f),
            m_pBrush
        );
    }

    // ========================================================================
    // Ліва панель: вибір джерела (USB ADB / Wi-Fi LAN), роздільна здатність, СТАРТ
    // ========================================================================
    void RenderLeftControlPanel(const UiRect& panel) {
        DrawRoundedCard(panel, COL_CARD_BG, COL_CARD_BORDER, 12.0f);

        float lx = panel.left + 16.0f;
        float rx = panel.right - 16.0f;
        float y = panel.top + 16.0f;

        // Заголовок панелі
        m_pBrush->SetColor(COL_TEXT_PRIMARY);
        m_pRenderTarget->DrawText(L"З'ЄДНАННЯ З ТЕЛЕФОНОМ", 21, m_pFormatBold, D2D1::RectF(lx, y, rx, y + 20.0f), m_pBrush);
        y += 28.0f;

        // Вкладки транспорту: [USB ADB] та [Wi-Fi LAN]
        m_rcTabUsb = { lx, y, lx + (rx - lx - 8.0f) * 0.5f, y + 36.0f };
        m_rcTabWifi = { m_rcTabUsb.right + 8.0f, y, rx, y + 36.0f };

        bool isUsb = (m_state.transport == TransportTab::USB_ADB);
        DrawRoundedCard(m_rcTabUsb, isUsb ? D2D1::ColorF(0.0f, 0.85f, 0.65f, 0.18f) : COL_CARD_BG,
                                    isUsb ? COL_ACCENT_PRIMARY : COL_CARD_BORDER, 8.0f);
        m_pBrush->SetColor(isUsb ? COL_ACCENT_PRIMARY : COL_TEXT_SECONDARY);
        m_pRenderTarget->DrawText(L"🔌 USB (ADB)", 12, m_pFormatBadge, D2D1::RectF(m_rcTabUsb.left, y + 9.0f, m_rcTabUsb.right, y + 27.0f), m_pBrush);

        DrawRoundedCard(m_rcTabWifi, !isUsb ? D2D1::ColorF(0.0f, 0.80f, 0.95f, 0.18f) : COL_CARD_BG,
                                     !isUsb ? COL_ACCENT_CYAN : COL_CARD_BORDER, 8.0f);
        m_pBrush->SetColor(!isUsb ? COL_ACCENT_CYAN : COL_TEXT_SECONDARY);
        m_pRenderTarget->DrawText(L"📶 Wi-Fi LAN", 12, m_pFormatBadge, D2D1::RectF(m_rcTabWifi.left, y + 9.0f, m_rcTabWifi.right, y + 27.0f), m_pBrush);
        y += 48.0f;

        if (isUsb) {
            // Випадаючий список підключених USB ADB смартфонів
            m_pBrush->SetColor(COL_TEXT_MUTED);
            m_pRenderTarget->DrawText(L"Вибір USB пристрою (ADB):", 25, m_pFormatRegular, D2D1::RectF(lx, y, rx, y + 18.0f), m_pBrush);
            y += 22.0f;

            m_rcUsbDropdown = { lx, y, rx, y + 40.0f };
            DrawRoundedCard(m_rcUsbDropdown, COL_CARD_BG, m_state.usbDropdownOpen ? COL_ACCENT_PRIMARY : COL_CARD_BORDER, 8.0f);

            std::wstring devLabel = L"Пошук пристроїв...";
            if (!m_state.usbDevices.empty() && m_state.selectedUsbDeviceIndex < static_cast<int>(m_state.usbDevices.size())) {
                const auto& d = m_state.usbDevices[m_state.selectedUsbDeviceIndex];
                std::string s = d.model;
                devLabel = std::wstring(s.begin(), s.end());
            } else if (m_state.usbDevices.empty()) {
                devLabel = L"Немає пристроїв (Увімкніть USB Debug)";
            }

            m_pBrush->SetColor(m_state.usbDevices.empty() ? COL_TEXT_MUTED : COL_TEXT_PRIMARY);
            m_pRenderTarget->DrawText(devLabel.c_str(), static_cast<UINT32>(devLabel.size()), m_pFormatBadge,
                D2D1::RectF(lx + 12.0f, y + 10.0f, rx - 32.0f, y + 30.0f), m_pBrush);

            // Стрілочка випадаючого списку
            m_pBrush->SetColor(COL_TEXT_SECONDARY);
            m_pRenderTarget->DrawText(m_state.usbDropdownOpen ? L"▲" : L"▼", 1, m_pFormatRegular,
                D2D1::RectF(rx - 24.0f, y + 10.0f, rx - 8.0f, y + 30.0f), m_pBrush);

            y += 50.0f;

            // Кнопка оновлення списку ADB
            m_rcBtnRefreshAdb = { lx, y, rx, y + 32.0f };
            DrawRoundedCard(m_rcBtnRefreshAdb, COL_CARD_BG, COL_CARD_BORDER, 6.0f);
            m_pBrush->SetColor(COL_TEXT_SECONDARY);
            m_pRenderTarget->DrawText(L"🔄 Оновити список пристроїв", 27, m_pFormatBadge,
                D2D1::RectF(lx, y + 7.0f, rx, y + 25.0f), m_pBrush);
            y += 46.0f;
        } else {
            // Введення IP адреси Wi-Fi
            m_pBrush->SetColor(COL_TEXT_MUTED);
            m_pRenderTarget->DrawText(L"IP адреса смартфона:", 20, m_pFormatRegular, D2D1::RectF(lx, y, rx, y + 18.0f), m_pBrush);
            y += 22.0f;

            m_rcIpInput = { lx, y, rx, y + 40.0f };
            DrawRoundedCard(m_rcIpInput, COL_CARD_BG, COL_CARD_BORDER, 8.0f);
            std::string ipStr = m_state.phoneIp + ":8554";
            std::wstring wip(ipStr.begin(), ipStr.end());
            m_pBrush->SetColor(COL_ACCENT_CYAN);
            m_pRenderTarget->DrawText(wip.c_str(), static_cast<UINT32>(wip.size()), m_pFormatMonospace,
                D2D1::RectF(lx + 12.0f, y + 10.0f, rx - 12.0f, y + 30.0f), m_pBrush);
            y += 54.0f;
        }

        // РОЗДІЛЬНА ЗДАТНІСТЬ (4K UHD, 2K QHD, 1080p 60, 720p 60)
        m_pBrush->SetColor(COL_TEXT_PRIMARY);
        m_pRenderTarget->DrawText(L"ЯКІСТЬ ПОТОКУ", 13, m_pFormatBold, D2D1::RectF(lx, y, rx, y + 20.0f), m_pBrush);
        y += 26.0f;

        const wchar_t* resLabels[4] = { L"4K UHD (2160p)", L"2K QHD (1440p)", L"1080p (60 FPS)", L"720p (60 FPS)" };
        float tabW = (rx - lx - 8.0f) * 0.5f;

        for (int i = 0; i < 4; ++i) {
            float col = static_cast<float>(i % 2);
            float row = static_cast<float>(i / 2);
            float bx = lx + col * (tabW + 8.0f);
            float by = y + row * 40.0f;

            m_rcResBtns[i] = { bx, by, bx + tabW, by + 34.0f };
            bool sel = (m_state.resPresetIdx == i);
            DrawRoundedCard(m_rcResBtns[i], sel ? D2D1::ColorF(0.0f, 0.85f, 0.65f, 0.16f) : COL_CARD_BG,
                                           sel ? COL_ACCENT_PRIMARY : COL_CARD_BORDER, 6.0f);
            m_pBrush->SetColor(sel ? COL_ACCENT_PRIMARY : COL_TEXT_SECONDARY);
            m_pRenderTarget->DrawText(resLabels[i], static_cast<UINT32>(wcslen(resLabels[i])), m_pFormatBadge,
                D2D1::RectF(bx, by + 8.0f, bx + tabW, by + 26.0f), m_pBrush);
        }
        y += 92.0f;

        // ГОЛОВНА КНОПКА СТАРТ / СТОП
        m_rcBtnToggleStream = { lx, panel.bottom - 60.0f, rx, panel.bottom - 16.0f };
        bool streaming = m_state.isStreaming;
        DrawRoundedCard(m_rcBtnToggleStream, streaming ? COL_ACCENT_DANGER : COL_ACCENT_PRIMARY,
                                            streaming ? COL_ACCENT_DANGER : COL_ACCENT_PRIMARY, 10.0f);

        const wchar_t* btnTxt = streaming ? L"⏹ ЗУПИНИТИ ТРАНСЛЯЦІЮ" : L"▶ ЗАПУСТИТИ КАМЕРУ";
        m_pBrush->SetColor(streaming ? COL_TEXT_PRIMARY : D2D1::ColorF(0.05f, 0.05f, 0.05f, 1.0f));
        m_pRenderTarget->DrawText(btnTxt, static_cast<UINT32>(wcslen(btnTxt)), m_pFormatBold,
            D2D1::RectF(lx, panel.bottom - 48.0f, rx, panel.bottom - 26.0f), m_pBrush);
    }

    // ========================================================================
    // Центральна панель: відеовікно 16:9, телеметрія FPS/бітрейт/пінг, сітка
    // ========================================================================
    void RenderCenterPreviewPanel(const UiRect& centerPanel) {
        DrawRoundedCard(centerPanel, COL_CARD_BG, COL_CARD_BORDER, 12.0f);

        // Відеов'юпорт 16:9 у центрі
        float vpMargin = 16.0f;
        float availW = centerPanel.Width() - (vpMargin * 2.0f);
        float availH = centerPanel.Height() - 110.0f;

        float videoW = availW;
        float videoH = videoW * (9.0f / 16.0f);
        if (videoH > availH) {
            videoH = availH;
            videoW = videoH * (16.0f / 9.0f);
        }

        float vx = centerPanel.left + (centerPanel.Width() - videoW) * 0.5f;
        float vy = centerPanel.top + vpMargin + (availH - videoH) * 0.5f;
        UiRect videoRect{ vx, vy, vx + videoW, vy + videoH };

        m_pBrush->SetColor(D2D1::ColorF(0.02f, 0.02f, 0.03f, 1.0f));
        m_pRenderTarget->FillRectangle(videoRect.ToD2D(), m_pBrush);

        bool hasFrame = false;
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            if (m_hasLiveFrame && m_pPreviewBitmap && !m_previewPixels.empty()) {
                m_pPreviewBitmap->CopyFromMemory(nullptr, m_previewPixels.data(), 640 * 4);
                m_pRenderTarget->DrawBitmap(m_pPreviewBitmap, videoRect.ToD2D());
                hasFrame = true;
            }
        }

        if (!hasFrame) {
            // Заглушка коли стрім вимкнено
            float midY = (videoRect.top + videoRect.bottom) * 0.5f;
            DrawLaptopEyeLogo((videoRect.left + videoRect.right) * 0.5f - 22.0f, midY - 68.0f, 40.0f);

            m_pBrush->SetColor(COL_TEXT_SECONDARY);
            const wchar_t* hint = m_state.isStreaming ? L"Підключення до смартфона..." : L"Натисніть «ЗАПУСТИТИ КАМЕРУ» для старту 4K потоку";
            m_pRenderTarget->DrawText(hint, static_cast<UINT32>(wcslen(hint)), m_pFormatBold,
                D2D1::RectF(videoRect.left, midY + 14.0f, videoRect.right, midY + 36.0f), m_pBrush);
        }

        // Відображення сітки третьої (Rule of Thirds Grid)
        if (m_state.showGrid) {
            m_pBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.25f));
            float g1x = vx + videoW * 0.333f;
            float g2x = vx + videoW * 0.666f;
            float g1y = vy + videoH * 0.333f;
            float g2y = vy + videoH * 0.666f;
            m_pRenderTarget->DrawLine(D2D1::Point2F(g1x, vy), D2D1::Point2F(g1x, vy + videoH), m_pBrush, 1.0f);
            m_pRenderTarget->DrawLine(D2D1::Point2F(g2x, vy), D2D1::Point2F(g2x, vy + videoH), m_pBrush, 1.0f);
            m_pRenderTarget->DrawLine(D2D1::Point2F(vx, g1y), D2D1::Point2F(vx + videoW, g1y), m_pBrush, 1.0f);
            m_pRenderTarget->DrawLine(D2D1::Point2F(vx, g2y), D2D1::Point2F(vx + videoW, g2y), m_pBrush, 1.0f);
        }

        // Рамка навколо відео
        m_pBrush->SetColor(COL_CARD_BORDER);
        m_pRenderTarget->DrawRectangle(videoRect.ToD2D(), m_pBrush, 1.5f);

        // НИЖНЯ СМУГА ТЕЛЕМЕТРІЇ: FPS, Бітрейт, Кадри, Затримка
        float by = centerPanel.bottom - 74.0f;
        UiRect barRect{ centerPanel.left + 16.0f, by, centerPanel.right - 16.0f, by + 58.0f };
        DrawRoundedCard(barRect, D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f), COL_CARD_BORDER, 8.0f);

        float itemW = barRect.Width() * 0.25f;

        // 1. FPS
        wchar_t buf[64]{};
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%u FPS", m_state.measuredFps);
        DrawTelemetryMetric(barRect.left, by, itemW, L"ЧАСТОТА КАДРІВ", buf, COL_ACCENT_PRIMARY);

        // 2. Бітрейт
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%.1f Mbps", static_cast<float>(m_state.measuredBitrateKbps) / 1000.0f);
        DrawTelemetryMetric(barRect.left + itemW, by, itemW, L"БІТРЕЙТ СТРІМУ", buf, COL_ACCENT_CYAN);

        // 3. Затримка (Latency)
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t), L"%.1f ms", m_state.currentRttMs);
        DrawTelemetryMetric(barRect.left + itemW * 2.0f, by, itemW, L"ЗАТРИМКА (RTT)", buf, COL_TEXT_PRIMARY);

        // 4. Назва моделі смартфона
        std::wstring wmodel(m_state.phoneModelName.begin(), m_state.phoneModelName.end());
        DrawTelemetryMetric(barRect.left + itemW * 3.0f, by, itemW, L"ПРИСТРІЙ", wmodel.c_str(), COL_TEXT_SECONDARY);
    }

    void DrawTelemetryMetric(float x, float y, float w, const wchar_t* title, const wchar_t* val, const D2D1_COLOR_F& col) {
        m_pBrush->SetColor(COL_TEXT_MUTED);
        m_pRenderTarget->DrawText(title, static_cast<UINT32>(wcslen(title)), m_pFormatBadge,
            D2D1::RectF(x + 10.0f, y + 8.0f, x + w - 10.0f, y + 24.0f), m_pBrush);

        m_pBrush->SetColor(col);
        m_pRenderTarget->DrawText(val, static_cast<UINT32>(wcslen(val)), m_pFormatBold,
            D2D1::RectF(x + 10.0f, y + 26.0f, x + w - 10.0f, y + 50.0f), m_pBrush);
    }

    // ========================================================================
    // Права панель: керування оптикою, модулями камер, зум (до 10x) та ISP Pro
    // ========================================================================
    void RenderRightOpticsPanel(const UiRect& panel) {
        DrawRoundedCard(panel, COL_CARD_BG, COL_CARD_BORDER, 12.0f);

        float rlx = panel.left + 16.0f;
        float rrx = panel.right - 16.0f;
        float y = panel.top + 16.0f;

        m_pBrush->SetColor(COL_TEXT_PRIMARY);
        m_pRenderTarget->DrawText(L"ОПТИКА ТА МОДУЛІ КАМЕР", 22, m_pFormatBold, D2D1::RectF(rlx, y, rrx, y + 20.0f), m_pBrush);
        y += 28.0f;

        // 7 кнопок пресетів зуму та об'єктивів: 0.5x, 1x, 2x, 3x, 5x, 10x, Front
        float btnW = (rrx - rlx - (6.0f * 4.0f)) / 7.0f;
        const wchar_t* lensLabels[7] = { L"0.5x", L"1x", L"2x", L"3x", L"5x", L"10x", L"Front" };
        const float lensZooms[7]     = { 0.5f,    1.0f,  2.0f,  3.0f,  5.0f,  10.0f,  1.0f };

        for (int i = 0; i < 7; ++i) {
            float bx = rlx + static_cast<float>(i) * (btnW + 4.0f);
            m_rcLensBtns[i] = { bx, y, bx + btnW, y + 36.0f };

            bool sel = (m_state.activeLensIdx == i);
            bool supported = true;
            if (i == 0 && m_state.phoneMinZoom >= 1.0f) supported = false;
            if (i >= 1 && i <= 5 && lensZooms[i] > m_state.phoneMaxZoom + 0.05f) supported = false;

            D2D1_COLOR_F fill = sel ? D2D1::ColorF(0.0f, 0.85f, 0.65f, 0.22f) : COL_CARD_BG;
            D2D1_COLOR_F brd = sel ? COL_ACCENT_PRIMARY : COL_CARD_BORDER;
            if (!supported) { fill = COL_CARD_BG; brd = D2D1::ColorF(0.14f, 0.14f, 0.16f, 1.0f); }

            DrawRoundedCard(m_rcLensBtns[i], fill, brd, 6.0f);
            m_pBrush->SetColor(sel ? COL_ACCENT_PRIMARY : (supported ? COL_TEXT_SECONDARY : COL_TEXT_MUTED));
            m_pRenderTarget->DrawText(lensLabels[i], static_cast<UINT32>(wcslen(lensLabels[i])), m_pFormatBadge,
                D2D1::RectF(bx, y + 9.0f, bx + btnW, y + 27.0f), m_pBrush);
        }
        y += 48.0f;

        // Повзунок плавного зуму (від phoneMinZoom до phoneMaxZoom, напр. 0.5x .. 10.0x)
        wchar_t zoomTitle[64]{};
        std::swprintf(zoomTitle, sizeof(zoomTitle) / sizeof(wchar_t), L"Плавний зум: %.1fx (макс. %.0fx)", m_state.currentZoom, m_state.phoneMaxZoom);
        m_pBrush->SetColor(COL_TEXT_SECONDARY);
        m_pRenderTarget->DrawText(zoomTitle, static_cast<UINT32>(wcslen(zoomTitle)), m_pFormatRegular, D2D1::RectF(rlx, y, rrx, y + 18.0f), m_pBrush);
        y += 22.0f;

        m_rcZoomSlider = { rlx, y, rrx, y + 24.0f };
        DrawSliderControl(m_rcZoomSlider, m_state.zoomSliderNorm, COL_ACCENT_PRIMARY);
        y += 42.0f;

        // Розділювач
        m_pBrush->SetColor(COL_CARD_BORDER);
        m_pRenderTarget->DrawLine(D2D1::Point2F(rlx, y), D2D1::Point2F(rrx, y), m_pBrush, 1.0f);
        y += 16.0f;

        // ПАНЕЛЬ РУЧНОГО КЕРУВАННЯ (ISP PRO MANUAL)
        m_pBrush->SetColor(COL_TEXT_PRIMARY);
        m_pRenderTarget->DrawText(L"РУЧНЕ НАЛАШТУВАННЯ (ISP)", 24, m_pFormatBold, D2D1::RectF(rlx, y, rrx - 100.0f, y + 20.0f), m_pBrush);

        // Перемикач Режиму AUTO / PRO
        float ispModeW = (rrx - rlx - 8.0f) * 0.58f;
        m_rcBtnModeToggle = { rrx - 90.0f, y - 2.0f, rrx, y + 22.0f };
        bool isPro = m_state.isProManual;
        DrawRoundedCard(m_rcBtnModeToggle, isPro ? D2D1::ColorF(0.0f, 0.80f, 0.95f, 0.2f) : COL_CARD_BG,
                                           isPro ? COL_ACCENT_CYAN : COL_CARD_BORDER, 6.0f);
        m_pBrush->SetColor(isPro ? COL_ACCENT_CYAN : COL_TEXT_SECONDARY);
        m_pRenderTarget->DrawText(isPro ? L"● PRO MANUAL" : L"○ AUTO ISP", isPro ? 12 : 10, m_pFormatBadge,
            D2D1::RectF(m_rcBtnModeToggle.left, y + 2.0f, m_rcBtnModeToggle.right, y + 18.0f), m_pBrush);
        y += 32.0f;

        // Повзунки ISO, Витримки, Фокусу та Балансу Білого
        const wchar_t* sliderNames[4] = { L"Чутливість (ISO)", L"Витримка (Shutter)", L"Фокус (Focus)", L"Баланс білого (WB)" };
        float sliderVals[4] = { m_state.isoNorm, m_state.shutterNorm, m_state.focusNorm, m_state.wbNorm };

        wchar_t sliderLabels[4][32]{};
        std::swprintf(sliderLabels[0], 32, L"ISO %d", 100 + static_cast<int>(m_state.isoNorm * 3100.0f));
        std::swprintf(sliderLabels[1], 32, L"1/%d с", 30 + static_cast<int>(m_state.shutterNorm * 3970.0f));
        std::swprintf(sliderLabels[2], 32, m_state.focusNorm == 0.0f ? L"Авто-фокус" : L"Ручний");
        std::swprintf(sliderLabels[3], 32, L"%d K", 2800 + static_cast<int>(m_state.wbNorm * 5700.0f));

        for (int i = 0; i < 4; ++i) {
            m_pBrush->SetColor(COL_TEXT_MUTED);
            m_pRenderTarget->DrawText(sliderNames[i], static_cast<UINT32>(wcslen(sliderNames[i])), m_pFormatRegular,
                D2D1::RectF(rlx, y, rlx + 160.0f, y + 16.0f), m_pBrush);

            m_pBrush->SetColor(COL_TEXT_SECONDARY);
            m_pRenderTarget->DrawText(sliderLabels[i], static_cast<UINT32>(wcslen(sliderLabels[i])), m_pFormatBadge,
                D2D1::RectF(rrx - 100.0f, y, rrx, y + 16.0f), m_pBrush);
            y += 18.0f;

            m_rcSliders[i] = { rlx, y, rrx, y + 18.0f };
            DrawSliderControl(m_rcSliders[i], sliderVals[i], isPro ? COL_ACCENT_CYAN : COL_TEXT_MUTED);
            y += 28.0f;
        }

        // Кнопка примусового перефокусування (Trigger Auto-Focus)
        m_rcBtnRefocus = { rlx, y, rrx, y + 32.0f };
        DrawRoundedCard(m_rcBtnRefocus, COL_CARD_BG, COL_CARD_BORDER, 6.0f);
        m_pBrush->SetColor(COL_ACCENT_PRIMARY);
        m_pRenderTarget->DrawText(L"🎯 Перефокусувати об'єктив", 26, m_pFormatBadge,
            D2D1::RectF(rlx, y + 7.0f, rrx, y + 25.0f), m_pBrush);
        y += 42.0f;

        // Перемикачі Дзеркало (Mirror) та Сітка (Grid)
        float halfW = (rrx - rlx - 8.0f) * 0.5f;
        m_rcBtnMirror = { rlx, y, rlx + halfW, y + 34.0f };
        m_rcBtnGrid = { rlx + halfW + 8.0f, y, rrx, y + 34.0f };

        DrawRoundedCard(m_rcBtnMirror, m_state.mirrorHorizontal ? D2D1::ColorF(0.0f, 0.85f, 0.65f, 0.16f) : COL_CARD_BG,
                                      m_state.mirrorHorizontal ? COL_ACCENT_PRIMARY : COL_CARD_BORDER, 6.0f);
        m_pBrush->SetColor(m_state.mirrorHorizontal ? COL_ACCENT_PRIMARY : COL_TEXT_SECONDARY);
        m_pRenderTarget->DrawText(L"🪞 Дзеркало", 10, m_pFormatBadge, D2D1::RectF(m_rcBtnMirror.left, y + 8.0f, m_rcBtnMirror.right, y + 26.0f), m_pBrush);

        DrawRoundedCard(m_rcBtnGrid, m_state.showGrid ? D2D1::ColorF(0.0f, 0.80f, 0.95f, 0.16f) : COL_CARD_BG,
                                    m_state.showGrid ? COL_ACCENT_CYAN : COL_CARD_BORDER, 6.0f);
        m_pBrush->SetColor(m_state.showGrid ? COL_ACCENT_CYAN : COL_TEXT_SECONDARY);
        m_pRenderTarget->DrawText(L"📐 Сітка", 8, m_pFormatBadge, D2D1::RectF(m_rcBtnGrid.left, y + 8.0f, m_rcBtnGrid.right, y + 26.0f), m_pBrush);
    }

    void DrawSliderControl(const UiRect& r, float norm, const D2D1_COLOR_F& col) {
        float midY = (r.top + r.bottom) * 0.5f;

        // Доріжка
        m_pBrush->SetColor(D2D1::ColorF(0.18f, 0.18f, 0.22f, 1.0f));
        m_pRenderTarget->DrawLine(D2D1::Point2F(r.left, midY), D2D1::Point2F(r.right, midY), m_pBrush, 4.0f);

        // Заповнена частина
        float fillX = r.left + r.Width() * norm;
        m_pBrush->SetColor(col);
        m_pRenderTarget->DrawLine(D2D1::Point2F(r.left, midY), D2D1::Point2F(fillX, midY), m_pBrush, 4.0f);

        // Повзунок (Thumb)
        m_pBrush->SetColor(col);
        m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fillX, midY), 7.0f, 7.0f), m_pBrush);
        m_pBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fillX, midY), 3.0f, 3.0f), m_pBrush);
    }

    void RenderUsbDropdownOverlay() {
        if (m_state.usbDevices.empty()) return;

        float itemH = 34.0f;
        float totalH = static_cast<float>(m_state.usbDevices.size()) * itemH + 8.0f;
        UiRect dropRect{ m_rcUsbDropdown.left, m_rcUsbDropdown.bottom + 4.0f, m_rcUsbDropdown.right, m_rcUsbDropdown.bottom + 4.0f + totalH };

        DrawRoundedCard(dropRect, D2D1::ColorF(0.13f, 0.13f, 0.17f, 1.0f), COL_ACCENT_PRIMARY, 8.0f);

        for (size_t i = 0; i < m_state.usbDevices.size(); ++i) {
            float iy = dropRect.top + 4.0f + static_cast<float>(i) * itemH;
            UiRect itemRect{ dropRect.left + 4.0f, iy, dropRect.right - 4.0f, iy + itemH };

            bool sel = (static_cast<int>(i) == m_state.selectedUsbDeviceIndex);
            if (sel) {
                m_pBrush->SetColor(D2D1::ColorF(0.0f, 0.85f, 0.65f, 0.2f));
                m_pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(itemRect.ToD2D(), 6.0f, 6.0f), m_pBrush);
            }

            std::string s = m_state.usbDevices[i].model;
            std::wstring ws(s.begin(), s.end());
            m_pBrush->SetColor(sel ? COL_ACCENT_PRIMARY : COL_TEXT_PRIMARY);
            m_pRenderTarget->DrawText(ws.c_str(), static_cast<UINT32>(ws.size()), m_pFormatBadge,
                D2D1::RectF(itemRect.left + 8.0f, iy + 7.0f, itemRect.right - 8.0f, iy + 25.0f), m_pBrush);
        }
    }

    void CreateTextFormats() {
        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI Variable Display", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            20.0f, L"uk-ua", &m_pFormatTitle
        );
        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            14.0f, L"uk-ua", &m_pFormatBold
        );
        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"uk-ua", &m_pFormatRegular
        );
        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            11.5f, L"uk-ua", &m_pFormatBadge
        );
        m_pDWriteFactory->CreateTextFormat(
            L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            13.0f, L"en-us", &m_pFormatMonospace
        );
    }

    void DiscardDeviceResources() {
        if (m_pPreviewBitmap) { m_pPreviewBitmap->Release(); m_pPreviewBitmap = nullptr; }
        if (m_pBrush) { m_pBrush->Release(); m_pBrush = nullptr; }
        if (m_pRenderTarget) { m_pRenderTarget->Release(); m_pRenderTarget = nullptr; }
    }

    // ========================================================================
    // Обробка миші та подій UI
    // ========================================================================
    void HandleMouseDown(float x, float y) {
        // Якщо відкрито випадаючий список пристроїв USB
        if (m_state.usbDropdownOpen) {
            float itemH = 34.0f;
            for (size_t i = 0; i < m_state.usbDevices.size(); ++i) {
                float iy = m_rcUsbDropdown.bottom + 4.0f + 4.0f + static_cast<float>(i) * itemH;
                UiRect itemRect{ m_rcUsbDropdown.left + 4.0f, iy, m_rcUsbDropdown.right - 4.0f, iy + itemH };
                if (itemRect.Contains(x, y)) {
                    m_state.selectedUsbDeviceIndex = static_cast<int>(i);
                    m_state.adbDeviceSerial = m_state.usbDevices[i].serial;
                    m_state.usbDropdownOpen = false;
                    return;
                }
            }
            m_state.usbDropdownOpen = false;
            return;
        }

        // Клік по кнопці реєстрації DirectShow
        if (x >= 1040.0f && x <= 1260.0f && y >= 18.0f && y <= 50.0f) {
            if (!m_state.isFilterRegistered) {
                if (ElevateAndRegisterDirectShowDriver(m_hwnd)) {
                    m_state.isFilterRegistered = true;
                    MessageBoxW(m_hwnd, L"EyeCam Virtual Camera DirectShow фільтр успішно зареєстровано в системі!", L"EyeCam Studio", MB_OK | MB_ICONINFORMATION);
                }
            }
            return;
        }

        // Перемикання вкладок транспорту (USB ADB / Wi-Fi)
        if (m_rcTabUsb.Contains(x, y)) {
            m_state.transport = TransportTab::USB_ADB;
            RefreshUsbDevicesAsync();
            return;
        }
        if (m_rcTabWifi.Contains(x, y)) {
            m_state.transport = TransportTab::WIFI_LAN;
            return;
        }

        // Випадаючий список ADB пристроїв
        if (m_state.transport == TransportTab::USB_ADB && m_rcUsbDropdown.Contains(x, y)) {
            m_state.usbDropdownOpen = !m_state.usbDropdownOpen;
            return;
        }
        if (m_state.transport == TransportTab::USB_ADB && m_rcBtnRefreshAdb.Contains(x, y)) {
            RefreshUsbDevicesAsync();
            return;
        }

        // Кнопка СТАРТ / СТОП
        if (m_rcBtnToggleStream.Contains(x, y)) {
            if (m_state.isStreaming) {
                StopStreaming();
            } else {
                StartStreaming();
            }
            return;
        }

        // Вибір пресету зуму / об'єктива (0.5x, 1x, 2x, 3x, 5x, 10x, Front)
        const char* lensNames[7] = { "0.5x", "1.0x", "2.0x", "3.0x", "5.0x", "10.0x", "front" };
        const float lensZooms[7] = { 0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f, 1.0f };
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

                char json[128]{};
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

        // Кнопка примусового автофокусування
        if (m_rcBtnRefocus.Contains(x, y)) {
            SendControlJson("{\"action\":\"TriggerAutoFocus\"}");
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
                m_activeSliderDrag = i;
                UpdateSliderValue(i, x);
                return;
            }
        }
    }

    void HandleMouseMove(float x, float y, WPARAM wParam) {
        if (wParam & MK_LBUTTON) {
            if (m_activeSliderDrag == 100) {
                UpdateZoomSliderValue(x);
            } else if (m_activeSliderDrag >= 0 && m_activeSliderDrag < 4) {
                UpdateSliderValue(m_activeSliderDrag, x);
            }
        }
    }

    void HandleMouseUp() {
        m_activeSliderDrag = -1;
    }

    void UpdateZoomSliderValue(float mouseX) {
        float norm = std::clamp((mouseX - m_rcZoomSlider.left) / m_rcZoomSlider.Width(), 0.0f, 1.0f);
        m_state.zoomSliderNorm = norm;
        float span = (std::max)(0.1f, m_state.phoneMaxZoom - m_state.phoneMinZoom);
        m_state.currentZoom = m_state.phoneMinZoom + norm * span;

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
            case WM_ERASEBKGND:
                return 1;

            case WM_SIZE:
                if (self->m_pRenderTarget) {
                    UINT32 w = (std::max)(1u, static_cast<UINT32>(LOWORD(lParam)));
                    UINT32 h = (std::max)(1u, static_cast<UINT32>(HIWORD(lParam)));
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
                self->RenderScene();
                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_LBUTTONDOWN:
                self->HandleMouseDown(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;

            case WM_MOUSEMOVE:
                self->HandleMouseMove(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)), wParam);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;

            case WM_LBUTTONUP:
                self->HandleMouseUp();
                return 0;

            case WM_APP + 42: {
                auto* fn = reinterpret_cast<std::function<void()>*>(wParam);
                if (fn) {
                    (*fn)();
                    delete fn;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

private:
    HWND m_hwnd = nullptr;
    StudioState m_state{};

    ID2D1Factory* m_pD2DFactory = nullptr;
    IDWriteFactory* m_pDWriteFactory = nullptr;
    ID2D1HwndRenderTarget* m_pRenderTarget = nullptr;
    ID2D1SolidColorBrush* m_pBrush = nullptr;
    ID2D1Bitmap* m_pPreviewBitmap = nullptr;

    IDWriteTextFormat* m_pFormatTitle = nullptr;
    IDWriteTextFormat* m_pFormatBold = nullptr;
    IDWriteTextFormat* m_pFormatRegular = nullptr;
    IDWriteTextFormat* m_pFormatBadge = nullptr;
    IDWriteTextFormat* m_pFormatMonospace = nullptr;

    EyeCam::WindowsDirectShowSink m_sink{};
    std::atomic<bool> m_running{ false };
    std::thread m_receiverThread;

    SOCKET m_phoneSocket = INVALID_SOCKET;
    std::mutex m_socketMutex;

    std::mutex m_frameMutex;
    std::vector<uint8_t> m_previewPixels;
    std::vector<uint8_t> m_cachedPreviewBuffer;
    std::vector<uint8_t> m_fullBgraBuffer;
    std::vector<uint8_t> m_yuy2Buffer;
    bool m_hasLiveFrame = false;

    int m_activeSliderDrag = -1;

    // UI координати інтерактивних компонентів
    UiRect m_rcTabUsb{};
    UiRect m_rcTabWifi{};
    UiRect m_rcUsbDropdown{};
    UiRect m_rcBtnRefreshAdb{};
    UiRect m_rcIpInput{};
    UiRect m_rcResBtns[4]{};
    UiRect m_rcBtnToggleStream{};
    UiRect m_rcLensBtns[7]{};
    UiRect m_rcZoomSlider{};
    UiRect m_rcBtnModeToggle{};
    UiRect m_rcBtnRefocus{};
    UiRect m_rcBtnMirror{};
    UiRect m_rcBtnGrid{};
    UiRect m_rcSliders[4]{};
};

#if !defined(EYECAM_LIB_ONLY)
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    EyeCamStudioWindow window;
    if (!window.Initialize(hInstance, nCmdShow)) {
        return -1;
    }
    window.RunMessageLoop();
    return 0;
}
#endif

#endif
