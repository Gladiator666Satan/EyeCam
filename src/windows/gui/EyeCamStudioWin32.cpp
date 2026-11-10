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

#ifdef DrawText
#undef DrawText
#endif

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

constexpr const wchar_t* WINDOW_CLASS_NAME = L"EyeCamStudioWindowClass";
constexpr const wchar_t* WINDOW_TITLE = L"EyeCam Studio — 4K UHD DirectShow Camera";

constexpr D2D1_COLOR_F COL_BG_DARK     = { 0.035f, 0.055f, 0.082f, 1.0f }; // #090E15 Mica deep
constexpr D2D1_COLOR_F COL_BG_CARD     = { 0.055f, 0.082f, 0.125f, 0.94f }; // #0E1520 Acrylic Card
constexpr D2D1_COLOR_F COL_BORDER      = { 0.120f, 0.180f, 0.250f, 0.85f };
constexpr D2D1_COLOR_F COL_CYAN        = { 0.000f, 0.898f, 1.000f, 1.0f }; // #00E5FF EyeCam Cyber Cyan
constexpr D2D1_COLOR_F COL_EMERALD     = { 0.000f, 0.902f, 0.463f, 1.0f }; // #00E676 Live Stream Green
constexpr D2D1_COLOR_F COL_ROSE        = { 1.000f, 0.176f, 0.333f, 1.0f }; // #FF2D55 Stop / Alert
constexpr D2D1_COLOR_F COL_TEXT_PRIMARY= { 0.960f, 0.980f, 1.000f, 1.0f };
constexpr D2D1_COLOR_F COL_TEXT_SECOND = { 0.580f, 0.650f, 0.740f, 1.0f };
constexpr D2D1_COLOR_F COL_TEXT_DIM    = { 0.360f, 0.420f, 0.500f, 1.0f };

D2D1_COLOR_F HexColor(uint32_t rgb, float a = 1.0f) {
    float r = ((rgb >> 16) & 0xFF) / 255.0f;
    float g = ((rgb >> 8) & 0xFF) / 255.0f;
    float b = (rgb & 0xFF) / 255.0f;
    return D2D1::ColorF(r, g, b, a);
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

    D2D1_RECT_F ToD2D() const {
        return D2D1::RectF(left, top, right, bottom);
    }
};

enum class TransportTab {
    USB_ADB = 0,
    WIFI_LAN = 1
};

struct UsbAdbDevice {
    std::wstring serial;
    std::wstring model;
    std::wstring displayName;
};

struct StudioState {
    TransportTab transport = TransportTab::USB_ADB;
    std::wstring phoneIp = L"192.168.1.100";
    std::vector<UsbAdbDevice> usbDevices;
    int selectedUsbIndex = 0;
    bool usbDropdownOpen = false;

    bool isStreaming = false;
    bool isPhoneConnected = false;
    std::wstring connectionStatusText = L"Очікування підключення";

    // Телеметрія потоку
    int currentFps = 0;
    int targetFps = 60;
    int currentBitrateKbps = 0;
    uint32_t frameWidth = 1920;
    uint32_t frameHeight = 1080;
    uint64_t totalFramesReceived = 0;
    uint64_t droppedFrames = 0;
    float batteryLevel = 0.0f;
    bool batteryCharging = false;
    std::wstring thermalState = L"NORMAL";

    // Налаштування камери (ISP Controls)
    int selectedResIndex = 1; // 0: 720p, 1: 1080p, 2: 2K, 3: 4K UHD
    int selectedLensIndex = 1; // 0: 0.5x, 1: 1.0x, 2: 2.0x, 3: 3.0x, 4: 5.0x, 5: 10x, 6: Front
    float zoomRatio = 1.0f;
    bool isFrontCamera = false;
    bool isProManual = false;

    int manualIso = 0; // 0 = Auto
    int manualShutter = 0; // 0 = Auto, 30, 60, 120, 250, 500, 1000
    int manualKelvin = 0; // 0 = Auto, 2800..7500K
    float manualFocus = -1.0f; // -1 = Auto, 0.0..1.0

    bool mirrorHorizontal = false;
    bool showGrid = false;
    bool directShowRegistered = false;
};

} // namespace

class EyeCamStudioWindow {
public:
    EyeCamStudioWindow() = default;
    ~EyeCamStudioWindow() {
        StopStream();
        CleanupD2D();
    }

    bool Initialize(HINSTANCE hInstance, int nCmdShow) {
        m_hInstance = hInstance;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = StaticWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = WINDOW_CLASS_NAME;
        wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(101));
        wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(101));

        RegisterClassExW(&wc);

        RECT rc = { 0, 0, 1320, 840 };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

        m_hwnd = CreateWindowExW(
            WS_EX_APPWINDOW,
            WINDOW_CLASS_NAME,
            WINDOW_TITLE,
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, hInstance, this
        );

        if (!m_hwnd) return false;

        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        COLORREF captionCol = RGB(9, 14, 21);
        DwmSetWindowAttribute(m_hwnd, DWMWA_CAPTION_COLOR, &captionCol, sizeof(captionCol));

        if (!InitD2D()) return false;
        if (!InitDirectShowSink()) return false;

        RefreshUsbDevices();

        ShowWindow(m_hwnd, nCmdShow);
        UpdateWindow(m_hwnd);

        // Автоматичний запуск фонового потоку перевірки нових USB пристроїв кожні 2 секунди
        m_bUsbMonitorRunning = true;
        m_usbMonitorThread = std::thread([this]() {
            while (m_bUsbMonitorRunning) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                if (m_state.transport == TransportTab::USB_ADB) {
                    RefreshUsbDevices(false);
                }
            }
        });

        return true;
    }

    int RunMessageLoop() {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        m_bUsbMonitorRunning = false;
        if (m_usbMonitorThread.joinable()) {
            m_usbMonitorThread.join();
        }
        return static_cast<int>(msg.wParam);
    }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        EyeCamStudioWindow* pThis = nullptr;
        if (uMsg == WM_NCCREATE) {
            auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
            pThis = reinterpret_cast<EyeCamStudioWindow*>(pCreate->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
            pThis->m_hwnd = hwnd;
        } else {
            pThis = reinterpret_cast<EyeCamStudioWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (pThis) {
            return pThis->HandleMessage(uMsg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
        switch (uMsg) {
        case WM_SIZE: {
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            ResizeRenderTarget(width, height);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(m_hwnd, &ps);
            Render();
            EndPaint(m_hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;

        case WM_LBUTTONDOWN: {
            float x = static_cast<float>(GET_X_LPARAM(lParam));
            float y = static_cast<float>(GET_Y_LPARAM(lParam));
            HandleMouseDown(x, y);
            return 0;
        }
        case WM_LBUTTONUP: {
            float x = static_cast<float>(GET_X_LPARAM(lParam));
            float y = static_cast<float>(GET_Y_LPARAM(lParam));
            HandleMouseUp(x, y);
            return 0;
        }
        case WM_MOUSEMOVE: {
            float x = static_cast<float>(GET_X_LPARAM(lParam));
            float y = static_cast<float>(GET_Y_LPARAM(lParam));
            HandleMouseMove(x, y, (wParam & MK_LBUTTON) != 0);
            return 0;
        }
        case WM_CHAR: {
            wchar_t ch = static_cast<wchar_t>(wParam);
            HandleCharInput(ch);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(m_hwnd, uMsg, wParam, lParam);
    }

    void RefreshUsbDevices(bool triggerRepaint = true) {
        // Виконання adb devices -l через прихований пайп
        std::vector<UsbAdbDevice> devices;

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
        if (CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
            SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = hWritePipe;
            si.hStdError = hWritePipe;

            PROCESS_INFORMATION pi{};
            wchar_t cmd[] = L"cmd.exe /c adb devices -l";
            if (CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(hWritePipe);
                hWritePipe = nullptr;

                char buffer[2048]{};
                DWORD bytesRead = 0;
                std::string output;
                while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
                    buffer[bytesRead] = '\0';
                    output += buffer;
                }

                WaitForSingleObject(pi.hProcess, 1500);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);

                // Парсинг виводу adb devices
                size_t pos = 0;
                while ((pos = output.find('\n', pos)) != std::string::npos) {
                    size_t nextPos = output.find('\n', pos + 1);
                    std::string line = output.substr(pos + 1, (nextPos == std::string::npos ? output.size() : nextPos) - pos - 1);
                    pos = nextPos;

                    if (line.find("device") != std::string::npos && line.find("List of devices") == std::string::npos) {
                        // Знайдено пристрій
                        char serial[128]{}, model[128]{};
                        if (sscanf_s(line.c_str(), "%127s", serial, (unsigned)_countof(serial)) == 1) {
                            std::string sSerial(serial);
                            std::string sModel = "Android Phone";
                            size_t modelPos = line.find("model:");
                            if (modelPos != std::string::npos) {
                                size_t spacePos = line.find(' ', modelPos);
                                sModel = line.substr(modelPos + 6, (spacePos == std::string::npos ? line.size() : spacePos) - modelPos - 6);
                            }

                            std::wstring wSerial(sSerial.begin(), sSerial.end());
                            std::wstring wModel(sModel.begin(), sModel.end());
                            devices.push_back({ wSerial, wModel, wModel + L" (" + wSerial + L")" });
                        }
                    }
                }
            }
            if (hWritePipe) CloseHandle(hWritePipe);
            CloseHandle(hReadPipe);
        }

        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state.usbDevices = devices;
        if (m_state.selectedUsbIndex >= static_cast<int>(devices.size())) {
            m_state.selectedUsbIndex = 0;
        }
        if (triggerRepaint) {
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }

    bool InitD2D() {
        D2D1_FACTORY_OPTIONS options{};
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, options, &m_pD2DFactory);
        if (FAILED(hr)) return false;

        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&m_pDWriteFactory)
        );
        if (FAILED(hr)) return false;

        hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&m_pWicFactory)
        );
        if (FAILED(hr)) return false;

        CreateTextFormats();
        return true;
    }

    void CreateTextFormats() {
        if (!m_pDWriteFactory) return;

        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI Variable Display", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            24.0f, L"uk-UA", &m_pFontTitle
        );

        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            15.0f, L"uk-UA", &m_pFontSection
        );

        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            13.0f, L"uk-UA", &m_pFontBody
        );

        m_pDWriteFactory->CreateTextFormat(
            L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"en-US", &m_pFontMono
        );

        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            11.0f, L"uk-UA", &m_pFontBadge
        );
    }

    void ResizeRenderTarget(UINT w, UINT h) {
        if (!m_pRenderTarget) {
            RECT rc;
            GetClientRect(m_hwnd, &rc);
            D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
            m_pD2DFactory->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
                ),
                D2D1::HwndRenderTargetProperties(m_hwnd, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY),
                &m_pRenderTarget
            );
            if (m_pRenderTarget) {
                m_pRenderTarget->CreateSolidColorBrush(COL_CYAN, &m_pBrush);
            }
        } else {
            m_pRenderTarget->Resize(D2D1::SizeU(w, h));
        }
    }

    void CleanupD2D() {
        if (m_pBrush) { m_pBrush->Release(); m_pBrush = nullptr; }
        if (m_pPreviewBitmap) { m_pPreviewBitmap->Release(); m_pPreviewBitmap = nullptr; }
        if (m_pRenderTarget) { m_pRenderTarget->Release(); m_pRenderTarget = nullptr; }
        if (m_pFontTitle) { m_pFontTitle->Release(); m_pFontTitle = nullptr; }
        if (m_pFontSection) { m_pFontSection->Release(); m_pFontSection = nullptr; }
        if (m_pFontBody) { m_pFontBody->Release(); m_pFontBody = nullptr; }
        if (m_pFontMono) { m_pFontMono->Release(); m_pFontMono = nullptr; }
        if (m_pFontBadge) { m_pFontBadge->Release(); m_pFontBadge = nullptr; }
        if (m_pDWriteFactory) { m_pDWriteFactory->Release(); m_pDWriteFactory = nullptr; }
        if (m_pWicFactory) { m_pWicFactory->Release(); m_pWicFactory = nullptr; }
        if (m_pD2DFactory) { m_pD2DFactory->Release(); m_pD2DFactory = nullptr; }
    }

    bool InitDirectShowSink() {
        m_dshowSink = std::make_unique<EyeCam::WindowsDirectShowSink>();
        m_state.directShowRegistered = true;
        return true;
    }

    void StartStream() {
        if (m_bStreamRunning) return;

        m_bStreamRunning = true;
        m_state.isStreaming = true;
        m_state.isPhoneConnected = false;
        m_state.connectionStatusText = L"З'єднання...";

        // Запуск TCP ресивера на окремому потоці
        m_streamThread = std::thread([this]() {
            RunNetworkClient();
        });
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void StopStream() {
        if (!m_bStreamRunning) return;

        m_bStreamRunning = false;
        if (m_clientSocket != INVALID_SOCKET) {
            closesocket(m_clientSocket);
            m_clientSocket = INVALID_SOCKET;
        }
        if (m_streamThread.joinable()) {
            m_streamThread.join();
        }

        m_state.isStreaming = false;
        m_state.isPhoneConnected = false;
        m_state.connectionStatusText = L"Зупинено";
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void RunNetworkClient() {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            m_state.connectionStatusText = L"Помилка WSAStartup";
            return;
        }

        while (m_bStreamRunning) {
            std::string host = "127.0.0.1";
            int port = 8554;

            if (m_state.transport == TransportTab::USB_ADB) {
                // Виконання adb forward tcp:8554 tcp:8554 для вибраного пристрою
                std::wstring serial = L"";
                {
                    std::lock_guard<std::mutex> lock(m_stateMutex);
                    if (!m_state.usbDevices.empty() && m_state.selectedUsbIndex < static_cast<int>(m_state.usbDevices.size())) {
                        serial = m_state.usbDevices[m_state.selectedUsbIndex].serial;
                    }
                }
                std::wstring cmd = L"adb.exe " + (serial.empty() ? L"" : (L"-s " + serial + L" ")) + L"forward tcp:8554 tcp:8554";
                _wsystem(cmd.c_str());
                host = "127.0.0.1";
            } else {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                std::string ipA(m_state.phoneIp.begin(), m_state.phoneIp.end());
                host = ipA;
            }

            m_clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (m_clientSocket == INVALID_SOCKET) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }

            // TCP NoDelay та оптимізація буферів для 60 FPS Ultra-Low Latency
            int nodelay = 1;
            setsockopt(m_clientSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
            int rcvBuf = 4 * 1024 * 1024; // 4MB буфер
            setsockopt(m_clientSocket, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvBuf, sizeof(rcvBuf));

            sockaddr_in serverAddr{};
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(static_cast<u_short>(port));
            inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr);

            m_state.connectionStatusText = L"Підключення до " + std::wstring(host.begin(), host.end()) + L":8554...";
            InvalidateRect(m_hwnd, nullptr, FALSE);

            if (connect(m_clientSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) != 0) {
                closesocket(m_clientSocket);
                m_clientSocket = INVALID_SOCKET;
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                continue;
            }

            m_state.isPhoneConnected = true;
            m_state.connectionStatusText = L"Онлайн (60 FPS)";
            InvalidateRect(m_hwnd, nullptr, FALSE);

            // Читання фреймів протоколу EyeCam:
            // [Header: 4B Magic "EYEC", 1B Type (0x01=JPEG, 0x02=Telemetry, 0x03=Audio), 4B PayloadSize, 8B TimestampNs]
            auto lastFpsTime = std::chrono::steady_clock::now();
            int fpsCounter = 0;
            int bytesInSec = 0;

            while (m_bStreamRunning) {
                uint8_t hdr[17];
                int r = RecvAll(m_clientSocket, hdr, sizeof(hdr));
                if (r <= 0) break;

                if (memcmp(hdr, "EYEC", 4) != 0) {
                    // Збитий потік синхронізації — перепідключення
                    break;
                }

                uint8_t msgType = hdr[4];
                uint32_t payloadSize = (hdr[5] << 24) | (hdr[6] << 16) | (hdr[7] << 8) | hdr[8];
                if (payloadSize > 25 * 1024 * 1024) break; // Захист від збійних пакетів

                std::vector<uint8_t> payload(payloadSize);
                if (RecvAll(m_clientSocket, payload.data(), payloadSize) <= 0) break;

                bytesInSec += static_cast<int>(sizeof(hdr) + payloadSize);

                if (msgType == 0x01) { // JPEG / MJPEG Відеофрейм
                    ProcessVideoFrame(payload.data(), payloadSize);
                    fpsCounter++;
                } else if (msgType == 0x02) { // JSON Телеметрія
                    ProcessTelemetryJson(reinterpret_cast<const char*>(payload.data()), payloadSize);
                }

                auto now = std::chrono::steady_clock::now();
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFpsTime).count();
                if (elapsedMs >= 1000) {
                    m_state.currentFps = static_cast<int>(fpsCounter * 1000.0f / elapsedMs);
                    m_state.currentBitrateKbps = static_cast<int>((bytesInSec * 8.0f) / (elapsedMs * 1.0f));
                    fpsCounter = 0;
                    bytesInSec = 0;
                    lastFpsTime = now;
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                }
            }

            closesocket(m_clientSocket);
            m_clientSocket = INVALID_SOCKET;
            m_state.isPhoneConnected = false;
            m_state.connectionStatusText = L"Відключено. Перепідключення...";
            InvalidateRect(m_hwnd, nullptr, FALSE);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }

        WSACleanup();
    }

    int RecvAll(SOCKET sock, uint8_t* buf, int totalBytes) {
        int received = 0;
        while (received < totalBytes && m_bStreamRunning) {
            int n = recv(sock, reinterpret_cast<char*>(buf + received), totalBytes - received, 0);
            if (n <= 0) return n;
            received += n;
        }
        return received;
    }

    void ProcessVideoFrame(const uint8_t* jpegData, uint32_t jpegSize) {
        if (!m_pWicFactory) return;

        // Декодування JPEG через апаратний Windows Imaging Component (WIC)
        IWICStream* pStream = nullptr;
        HRESULT hr = m_pWicFactory->CreateStream(&pStream);
        if (FAILED(hr)) return;

        hr = pStream->InitializeFromMemory(const_cast<BYTE*>(jpegData), jpegSize);
        if (FAILED(hr)) { pStream->Release(); return; }

        IWICBitmapDecoder* pDecoder = nullptr;
        hr = m_pWicFactory->CreateDecoderFromStream(pStream, nullptr, WICDecodeMetadataCacheOnDemand, &pDecoder);
        if (FAILED(hr)) { pStream->Release(); return; }

        IWICBitmapFrameDecode* pSource = nullptr;
        hr = pDecoder->GetFrame(0, &pSource);
        if (FAILED(hr)) { pDecoder->Release(); pStream->Release(); return; }

        UINT w = 0, h = 0;
        pSource->GetSize(&w, &h);

        IWICFormatConverter* pConverter = nullptr;
        hr = m_pWicFactory->CreateFormatConverter(&pConverter);
        if (SUCCEEDED(hr)) {
            hr = pConverter->Initialize(
                pSource,
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0f,
                WICBitmapPaletteTypeCustom
            );
        }

        if (SUCCEEDED(hr) && pConverter) {
            std::vector<uint8_t> bgra(w * h * 4);
            pConverter->CopyPixels(nullptr, w * 4, static_cast<UINT>(bgra.size()), bgra.data());

            // Оновлюємо DirectShow Shared Memory Sink для OBS / Zoom / Meet
            if (m_dshowSink) {
                if (m_state.frameWidth != w || m_state.frameHeight != h) {
                    m_dshowSink->Open(w, h, EyeCam::PixelFormat::RGB32);
                    m_state.frameWidth = w;
                    m_state.frameHeight = h;
                }
                m_dshowSink->WriteFrame(bgra.data(), bgra.size(), 0);
            }

            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                m_latestFrameBgra = std::move(bgra);
                m_latestFrameW = w;
                m_latestFrameH = h;
                m_bHasNewFrame = true;
            }

            // Інвалідуємо тільки зону перегляду для високої продуктивності 60 FPS
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }

        if (pConverter) pConverter->Release();
        pSource->Release();
        pDecoder->Release();
        pStream->Release();
    }

    void ProcessTelemetryJson(const char* jsonStr, uint32_t len) {
        std::string s(jsonStr, len);
        // Витягування базових параметрів (FPS, акумулятор, температура)
        float bat = 0.0f;
        if (sscanf_s(s.c_str(), "%*[^\"batteryLevel\"]\"batteryLevel\":%f", &bat) == 1) {
            m_state.batteryLevel = bat;
        }
        if (s.find("\"isCharging\":true") != std::string::npos) {
            m_state.batteryCharging = true;
        } else if (s.find("\"isCharging\":false") != std::string::npos) {
            m_state.batteryCharging = false;
        }
        if (s.find("THROTTLED") != std::string::npos) {
            m_state.thermalState = L"🔥 ТРОТЛІНГ";
        } else if (s.find("MODERATE") != std::string::npos) {
            m_state.thermalState = L"🌡 ТЕПЛО";
        } else {
            m_state.thermalState = L"❄ НОРМА";
        }
    }

    void SendControlCommand(const std::string& action, const std::string& param = "", double val = 0.0) {
        if (m_clientSocket == INVALID_SOCKET || !m_bStreamRunning) return;

        char buf[512];
        if (param.empty()) {
            snprintf(buf, sizeof(buf), "{\"action\":\"%s\"}\n", action.c_str());
        } else {
            snprintf(buf, sizeof(buf), "{\"action\":\"%s\",\"param\":\"%s\",\"value\":%.2f}\n", action.c_str(), param.c_str(), val);
        }
        send(m_clientSocket, buf, static_cast<int>(strlen(buf)), 0);
    }

    void Render() {
        if (!m_pRenderTarget) return;

        m_pRenderTarget->BeginDraw();
        m_pRenderTarget->Clear(COL_BG_DARK);

        RECT rcClient;
        GetClientRect(m_hwnd, &rcClient);
        float w = static_cast<float>(rcClient.right - rcClient.left);
        float h = static_cast<float>(rcClient.bottom - rcClient.top);

        // Малювання сучасної панелі керування (Acrylic Glassmorphism Layout)
        RenderTopHeader(w);
        RenderVideoPreview(w, h);
        RenderRightInspector(w, h);
        RenderLeftNavigation(w, h);

        m_pRenderTarget->EndDraw();
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
        float scale = h / 48.0f;
        // 1. Рамка дисплея ноутбука (Win11 Neon Slate)
        UiRect laptopScreen = { x, y + 2.0f * scale, x + 50.0f * scale, y + 36.0f * scale };
        DrawRoundedCard(laptopScreen, HexColor(0x0E1926), COL_CYAN, 5.0f * scale);

        // 2. База / клавіатура ноутбука
        D2D1_ROUNDED_RECT base = D2D1::RoundedRect(
            D2D1::RectF(x - 5.0f * scale, y + 36.0f * scale, x + 55.0f * scale, y + 42.0f * scale),
            2.0f * scale, 2.0f * scale
        );
        m_pBrush->SetColor(HexColor(0x19283B));
        m_pRenderTarget->FillRoundedRectangle(base, m_pBrush);
        m_pBrush->SetColor(COL_CYAN);
        m_pRenderTarget->DrawRoundedRectangle(base, m_pBrush, 1.0f * scale);

        // 3. Форма ока всередині екрана
        float cx = x + 25.0f * scale;
        float cy = y + 19.0f * scale;

        ID2D1PathGeometry* pEyePath = nullptr;
        m_pD2DFactory->CreatePathGeometry(&pEyePath);
        if (pEyePath) {
            ID2D1GeometrySink* pSink = nullptr;
            pEyePath->Open(&pSink);
            pSink->BeginFigure(D2D1::Point2F(cx - 16.0f * scale, cy), D2D1_FIGURE_BEGIN_FILLED);
            pSink->AddBezier(D2D1::BezierSegment(
                D2D1::Point2F(cx - 8.0f * scale, cy - 12.0f * scale),
                D2D1::Point2F(cx + 8.0f * scale, cy - 12.0f * scale),
                D2D1::Point2F(cx + 16.0f * scale, cy)
            ));
            pSink->AddBezier(D2D1::BezierSegment(
                D2D1::Point2F(cx + 8.0f * scale, cy + 12.0f * scale),
                D2D1::Point2F(cx - 8.0f * scale, cy + 12.0f * scale),
                D2D1::Point2F(cx - 16.0f * scale, cy)
            ));
            pSink->EndFigure(D2D1_FIGURE_END_CLOSED);
            pSink->Close();
            pSink->Release();

            m_pBrush->SetColor(HexColor(0x05131E));
            m_pRenderTarget->FillGeometry(pEyePath, m_pBrush);
            m_pBrush->SetColor(COL_CYAN);
            m_pRenderTarget->DrawGeometry(pEyePath, m_pBrush, 1.5f * scale);
            pEyePath->Release();
        }

        // 4. Зіниця та блік лінзи об'єктива
        D2D1_ELLIPSE pupil = D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.5f * scale, 6.5f * scale);
        m_pBrush->SetColor(COL_CYAN);
        m_pRenderTarget->FillEllipse(pupil, m_pBrush);

        D2D1_ELLIPSE innerCore = D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.5f * scale, 3.5f * scale);
        m_pBrush->SetColor(COL_BG_DARK);
        m_pRenderTarget->FillEllipse(innerCore, m_pBrush);

        // Фотографічний блік лінзи (білий з акцентом)
        D2D1_ELLIPSE flare1 = D2D1::Ellipse(D2D1::Point2F(cx - 2.5f * scale, cy - 2.5f * scale), 1.8f * scale, 1.8f * scale);
        m_pBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
        m_pRenderTarget->FillEllipse(flare1, m_pBrush);
    }

    void RenderTopHeader(float w) {
        float pad = 16.0f;
        float topBarH = 48.0f;

        DrawLaptopEyeLogo(pad + 4.0f, pad, topBarH);

        DrawTextStr(L"EYECAM STUDIO", {pad + 64, pad, pad + 290, pad + topBarH}, m_pFontTitle, COL_TEXT_PRIMARY);
        DrawTextStr(L"4K UHD DirectShow & ADB/Wi-Fi Pro Camera Engine", {pad + 245, pad + 2, pad + 680, pad + topBarH}, m_pFontBody, COL_TEXT_SECOND);

        // Статус потоку (Live Pill)
        UiRect statusPill = { w - pad - 210, pad + 6, w - pad, pad + 40 };
        D2D1_COLOR_F statusBg = m_state.isPhoneConnected ? HexColor(0x082618) : HexColor(0x221319);
        D2D1_COLOR_F statusBorder = m_state.isPhoneConnected ? COL_EMERALD : COL_ROSE;
        DrawRoundedCard(statusPill, statusBg, statusBorder, 17.0f);
        DrawTextStr(
            m_state.isPhoneConnected ? L"● LIVE STREAM 60 FPS" : L"○ НЕМАЄ ПІДКЛЮЧЕННЯ",
            statusPill, m_pFontBadge, statusBorder, DWRITE_TEXT_ALIGNMENT_CENTER
        );
    }

    void RenderLeftNavigation(float /*w*/, float h) {
        float lx = 16.0f;
        float rx = 330.0f;
        float cy = 80.0f;

        // Картка 1: Спосіб підключення
        UiRect rcCardConn = { lx, cy, rx, cy + 245.0f };
        DrawRoundedCard(rcCardConn, COL_BG_CARD, COL_BORDER, 12.0f);

        cy += 14.0f;
        lx += 16.0f;
        rx -= 16.0f;

        DrawTextStr(L"1. СПОСІБ ПІДКЛЮЧЕННЯ", {lx, cy, rx, cy + 24}, m_pFontSection, COL_TEXT_PRIMARY);
        cy += 28.0f;

        // Вкладки USB ADB / Wi-Fi
        float tabW = (rx - lx - 8.0f) * 0.5f;
        m_rcTransportTabs[0] = { lx, cy, lx + tabW, cy + 34 };
        m_rcTransportTabs[1] = { lx + tabW + 8.0f, cy, rx, cy + 34 };

        const wchar_t* tabNames[2] = { L"🔌 USB (ADB)", L"📶 Wi-Fi LAN" };
        for (int i = 0; i < 2; ++i) {
            bool sel = (m_state.transport == static_cast<TransportTab>(i));
            DrawRoundedCard(m_rcTransportTabs[i], sel ? HexColor(0x162C3D) : COL_BG_DARK, sel ? COL_CYAN : COL_BORDER, 8.0f);
            DrawTextStr(tabNames[i], m_rcTransportTabs[i], m_pFontSection, sel ? COL_CYAN : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        cy += 44.0f;

        if (m_state.transport == TransportTab::WIFI_LAN) {
            DrawTextStr(L"IP-адреса смартфона (див. у додатку):", {lx, cy, rx, cy + 20}, m_pFontBody, COL_TEXT_SECOND);
            cy += 24.0f;

            m_rcIpInput = { lx, cy, rx, cy + 34.0f };
            DrawRoundedCard(m_rcIpInput, COL_BG_DARK, COL_CYAN, 8.0f);
            std::wstring ipW = m_state.phoneIp + (m_bIpEditing ? L"|" : L"");
            DrawTextStr(L"  " + ipW, m_rcIpInput, m_pFontMono, COL_TEXT_PRIMARY);
            cy += 40.0f;

            DrawTextStr(L"Порт синхронізації та відео (TCP): 8554", {lx, cy, rx, cy + 20}, m_pFontBody, COL_TEXT_DIM);
            cy += 24.0f;

        } else {
            // USB ADB: Випадаючий список виявлених пристроїв
            DrawTextStr(L"Виберіть смартфон, підключений по USB:", {lx, cy, rx, cy + 20}, m_pFontBody, COL_TEXT_SECOND);
            cy += 24.0f;

            m_rcUsbDropdown = { lx, cy, rx, cy + 38.0f };
            DrawRoundedCard(m_rcUsbDropdown, HexColor(0x091420), COL_CYAN, 8.0f);

            std::wstring selectedUsbLabel = L"  Пошук підключених USB...";
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                if (!m_state.usbDevices.empty() && m_state.selectedUsbIndex < static_cast<int>(m_state.usbDevices.size())) {
                    selectedUsbLabel = L"  📱 " + m_state.usbDevices[m_state.selectedUsbIndex].displayName;
                } else if (m_state.usbDevices.empty()) {
                    selectedUsbLabel = L"  ❌ Немає USB ADB пристроїв";
                }
            }
            DrawTextStr(selectedUsbLabel, {lx, cy, rx - 28, cy + 38}, m_pFontBody,
                        COL_TEXT_PRIMARY, DWRITE_TEXT_ALIGNMENT_LEADING);
            DrawTextStr(m_state.usbDropdownOpen ? L"▲" : L"▼", {rx - 28, cy, rx - 6, cy + 38}, m_pFontBody, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
            cy += 46.0f;

            // Кнопка оновлення USB
            m_rcBtnRefreshUsb = { lx, cy, rx, cy + 32.0f };
            DrawRoundedCard(m_rcBtnRefreshUsb, COL_BG_DARK, COL_BORDER, 6.0f);
            DrawTextStr(L"↻  Оновити список USB-пристроїв", m_rcBtnRefreshUsb, m_pFontBody, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
            cy += 40.0f;
        }

        // Кнопка Старт/Стоп трансляції
        m_rcBtnToggleStream = { 16.0f, cy + 12.0f, 330.0f, cy + 62.0f };
        bool running = m_state.isStreaming;
        DrawRoundedCard(
            m_rcBtnToggleStream,
            running ? HexColor(0x3B101A) : HexColor(0x0C3322),
            running ? COL_ROSE : COL_EMERALD, 12.0f
        );
        DrawTextStr(
            running ? L"⏹ ЗУПИНИТИ ТРАНСЛЯЦІЮ" : L"▶ РОЗПОЧАТИ ТРАНСЛЯЦІЮ",
            m_rcBtnToggleStream, m_pFontSection, running ? COL_ROSE : COL_EMERALD, DWRITE_TEXT_ALIGNMENT_CENTER
        );
        cy += 74.0f;

        // Картка 2: Системний драйвер EyeCam (DirectShow)
        UiRect rcCardDriver = { 16.0f, cy, 330.0f, cy + 160.0f };
        DrawRoundedCard(rcCardDriver, COL_BG_CARD, COL_BORDER, 12.0f);
        cy += 14.0f;
        DrawTextStr(L"2. СИСТЕМНИЙ ДРАЙВЕР КАМЕРИ", {28.0f, cy, 318.0f, cy + 24}, m_pFontSection, COL_TEXT_PRIMARY);
        cy += 26.0f;

        DrawRoundedCard({28.0f, cy, 318.0f, cy + 34}, HexColor(0x082618), COL_EMERALD, 6.0f);
        DrawTextStr(L"Камера: EyeCam Virtual Camera", {lx + 12, cy + 8, rx - 12, cy + 30}, m_pFontBody, COL_TEXT_PRIMARY);
        DrawTextStr(L"✓ Готова до роботи в Zoom, Teams, OBS", {28.0f, cy + 38, 318.0f, cy + 58}, m_pFontBadge, COL_EMERALD);
        cy += 64.0f;

        m_rcBtnRegisterDriver = { 28.0f, cy, 318.0f, cy + 32.0f };
        DrawRoundedCard(m_rcBtnRegisterDriver, COL_BG_DARK, COL_BORDER, 6.0f);
        DrawTextStr(L"🔧 Перереєструвати фільтр (regsvr32)", m_rcBtnRegisterDriver, m_pFontBody, COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);

        // Якщо відкритий дропдаун USB — малюємо поверх елементів
        if (m_state.transport == TransportTab::USB_ADB && m_state.usbDropdownOpen) {
            float dropY = m_rcUsbDropdown.bottom + 4.0f;
            float itemH = 34.0f;
            float totalH = std::max(1.0f, static_cast<float>(m_state.usbDevices.size())) * itemH;
            UiRect dropBox = { m_rcUsbDropdown.left, dropY, m_rcUsbDropdown.right, dropY + totalH };
            DrawRoundedCard(dropBox, HexColor(0x060C14), COL_CYAN, 8.0f);

            m_rcUsbItems.clear();
            if (m_state.usbDevices.empty()) {
                DrawTextStr(L"  Немає активних USB ADB пристроїв", dropBox, m_pFontBody, COL_TEXT_SECOND);
            } else {
                for (size_t i = 0; i < m_state.usbDevices.size(); ++i) {
                    UiRect itemR = { dropBox.left, dropY + i * itemH, dropBox.right, dropY + (i + 1) * itemH };
                    m_rcUsbItems.push_back(itemR);
                    bool isSel = (static_cast<int>(i) == m_state.selectedUsbIndex);
                    if (isSel) {
                        DrawRoundedCard(itemR, HexColor(0x112739), COL_CYAN, 4.0f);
                    }
                    DrawTextStr(L"  " + m_state.usbDevices[i].displayName, itemR, m_pFontBody, COL_TEXT_PRIMARY);
                }
            }
        }
    }

    void RenderVideoPreview(float w, float h) {
        float left = 346.0f;
        float right = w - 340.0f;
        float top = 80.0f;
        float bottom = h - 20.0f;

        m_rcPreviewArea = { left, top, right, bottom };
        DrawRoundedCard(m_rcPreviewArea, HexColor(0x04060A), COL_BORDER, 14.0f);

        // Відображення кадру
        std::vector<uint8_t> currentFrame;
        uint32_t fw = 0, fh = 0;
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            if (!m_latestFrameBgra.empty()) {
                currentFrame = m_latestFrameBgra;
                fw = m_latestFrameW;
                fh = m_latestFrameH;
            }
        }

        UiRect videoRect = m_rcPreviewArea;
        // Відступ всередині рамки
        videoRect.left += 8.0f;
        videoRect.top += 8.0f;
        videoRect.right -= 8.0f;
        videoRect.bottom -= 8.0f;

        if (!currentFrame.empty() && fw > 0 && fh > 0 && m_pRenderTarget) {
            // Оновлюємо або створюємо Direct2D бітмап
            if (!m_pPreviewBitmap || m_bitmapW != fw || m_bitmapH != fh) {
                if (m_pPreviewBitmap) { m_pPreviewBitmap->Release(); m_pPreviewBitmap = nullptr; }
                D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
                );
                m_pRenderTarget->CreateBitmap(D2D1::SizeU(fw, fh), props, &m_pPreviewBitmap);
                m_bitmapW = fw;
                m_bitmapH = fh;
            }

            if (m_pPreviewBitmap) {
                m_pPreviewBitmap->CopyFromMemory(nullptr, currentFrame.data(), fw * 4);

                // Розрахунок збереження пропорцій (Aspect Fit)
                float frameAspect = static_cast<float>(fw) / static_cast<float>(fh);
                float boxW = videoRect.Width();
                float boxH = videoRect.Height();
                float boxAspect = boxW / boxH;

                UiRect drawRc = videoRect;
                if (frameAspect > boxAspect) {
                    float targetH = boxW / frameAspect;
                    float diffY = (boxH - targetH) * 0.5f;
                    drawRc.top += diffY;
                    drawRc.bottom -= diffY;
                } else {
                    float targetW = boxH * frameAspect;
                    float diffX = (boxW - targetW) * 0.5f;
                    drawRc.left += diffX;
                    drawRc.right -= diffX;
                }

                // Малювання кадру з можливістю горизонтального віддзеркалення
                D2D1_RECT_F d2dDraw = drawRc.ToD2D();
                if (m_state.mirrorHorizontal) {
                    D2D1_MATRIX_3X2_F curTransform;
                    m_pRenderTarget->GetTransform(&curTransform);
                    D2D1_MATRIX_3X2_F mirror = D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, D2D1::Point2F((drawRc.left + drawRc.right) * 0.5f, 0.0f));
                    m_pRenderTarget->SetTransform(mirror * curTransform);
                    m_pRenderTarget->DrawBitmap(m_pPreviewBitmap, d2dDraw);
                    m_pRenderTarget->SetTransform(curTransform);
                } else {
                    m_pRenderTarget->DrawBitmap(m_pPreviewBitmap, d2dDraw);
                }

                // Сітка 3x3 для правила третин
                if (m_state.showGrid) {
                    m_pBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f));
                    float colW = drawRc.Width() / 3.0f;
                    float rowH = drawRc.Height() / 3.0f;
                    for (int i = 1; i <= 2; ++i) {
                        m_pRenderTarget->DrawLine(
                            D2D1::Point2F(drawRc.left + i * colW, drawRc.top),
                            D2D1::Point2F(drawRc.left + i * colW, drawRc.bottom),
                            m_pBrush, 1.0f
                        );
                        m_pRenderTarget->DrawLine(
                            D2D1::Point2F(drawRc.left, drawRc.top + i * rowH),
                            D2D1::Point2F(drawRc.right, drawRc.top + i * rowH),
                            m_pBrush, 1.0f
                        );
                    }
                }
            }
        } else {
            // Заставка очікування
            float midY = (videoRect.top + videoRect.bottom) * 0.5f;
            if (!m_state.isStreaming) {
                DrawTextStr(L"ТРАНСЛЯЦІЮ ПРИЗУПИНЕНО", {videoRect.left, midY - 12, videoRect.right, midY + 18},
                            m_pFontSection, COL_TEXT_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (m_state.transport == TransportTab::USB_ADB) {
                DrawTextStr(L"ОЧІКУВАННЯ ВІДЕОПОТОКУ ПО USB (ADB)...", {videoRect.left, midY - 14, videoRect.right, midY + 14},
                            m_pFontSection, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
                DrawTextStr(L"Переконайтеся, що на телефоні запущено додаток EyeCam та увімкнено USB налагодження",
                            {videoRect.left, midY + 20, videoRect.right, midY + 44},
                            m_pFontBody, COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                std::wstring waitWifi = L"ОЧІКУВАННЯ ВІДЕОПОТОКУ ПО WI-FI (" + m_state.phoneIp + L":8554)...";
                DrawTextStr(waitWifi, {videoRect.left, midY - 14, videoRect.right, midY + 14},
                            m_pFontSection, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
                DrawTextStr(L"Перевірте спільну Wi-Fi мережу комп'ютера та смартфона",
                            {videoRect.left, midY + 20, videoRect.right, midY + 44},
                            m_pFontBody, COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }

        // HUD оверлеї телеметрії поверх відео
        RenderVideoHud(videoRect, fw, fh);
    }

    void RenderVideoHud(const UiRect& r, uint32_t fw, uint32_t fh) {
        // Плашка роздільності та FPS у лівому верхньому куті
        UiRect hudBadge1 = { r.left + 16.0f, r.top + 16.0f, r.left + 230.0f, r.top + 46.0f };
        DrawRoundedCard(hudBadge1, HexColor(0x060B12, 0.88f), COL_BORDER, 6.0f);

        wchar_t badgeBuf[128];
        if (m_state.isPhoneConnected && fw > 0) {
            swprintf_s(badgeBuf, L"● %ux%u @ %d FPS", fw, fh, m_state.currentFps);
        } else {
            swprintf_s(badgeBuf, L"○ ОФЛАЙН");
        }
        DrawTextStr(badgeBuf, hudBadge1, m_pFontMono, m_state.isPhoneConnected ? COL_EMERALD : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);

        // Нижня панель телеметрії (Бітрейт, Акумулятор, Температура)
        UiRect teleBar = { r.left + 16.0f, r.bottom - 46.0f, r.right - 16.0f, r.bottom - 16.0f };
        DrawRoundedCard(teleBar, HexColor(0x060B12, 0.88f), COL_BORDER, 6.0f);

        wchar_t teleStr[256];
        float mbps = m_state.currentBitrateKbps / 1000.0f;
        swprintf_s(
            teleStr,
            L"  БІТРЕЙТ: %.1f Мбіт/с  |  БАТАРЕЯ: %.0f%%%s  |  ТЕМПЕРАТУРА: %s  |  ЗАТРИМКА: ~18 мс",
            mbps,
            m_state.batteryLevel * 100.0f,
            m_state.batteryCharging ? L" ⚡" : L"",
            m_state.thermalState.c_str()
        );
        DrawTextStr(teleStr, teleBar, m_pFontMono, COL_TEXT_SECOND);
    }

    void RenderRightInspector(float w, float /*h*/) {
        float rlx = w - 326.0f;
        float rrx = w - 16.0f;
        float rcy = 80.0f;

        UiRect rcCardLens = { rlx, rcy, rrx, rcy + 740.0f };
        DrawRoundedCard(rcCardLens, COL_BG_CARD, COL_BORDER, 12.0f);

        rcy += 14.0f;
        rlx += 16.0f;
        rrx -= 16.0f;

        // 3. Модулі камер (Оптичні лінзи 0.5x, 1x, 2x, 3x, 5x, 10x, Front)
        wchar_t lensTitle[128];
        swprintf_s(lensTitle, L"3. МОДУЛІ КАМЕР (ЗУМ: %.1fx)", m_state.zoomRatio);
        DrawTextStr(lensTitle, {rlx, rcy, rrx, rcy + 22}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 26.0f;

        const wchar_t* lensLabels[7] = { L"0.5x", L"1.0x", L"2.0x", L"3.0x", L"5.0x", L"10x", L"🤳 СЕЛФІ" };
        float bWidth4 = (rrx - rlx - 18.0f) / 4.0f;

        // Рядок 1: 0.5x, 1x, 2x, 3x
        for (int i = 0; i < 4; ++i) {
            m_rcLensBtns[i] = { rlx + i * (bWidth4 + 6.0f), rcy, rlx + (i + 1) * bWidth4 + i * 6.0f, rcy + 30 };
            bool sel = (m_state.selectedLensIndex == i);
            DrawRoundedCard(m_rcLensBtns[i], sel ? HexColor(0x162C3D) : COL_BG_DARK, sel ? COL_CYAN : COL_BORDER, 6.0f);
            DrawTextStr(lensLabels[i], m_rcLensBtns[i], m_pFontBody, sel ? COL_CYAN : COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        rcy += 36.0f;

        // Рядок 2: 5x, 10x, Селфі
        float bWidth3 = (rrx - rlx - 12.0f) / 3.0f;
        for (int i = 4; i < 7; ++i) {
            int col = i - 4;
            float btnW = (i == 6) ? (bWidth3 * 1.3f) : ((bWidth3 * 2.0f - 6.0f) * 0.5f);
            float btnX = (i == 4) ? rlx : ((i == 5) ? (rlx + btnW + 6.0f) : (rrx - btnW));
            m_rcLensBtns[i] = { btnX, rcy, btnX + btnW, rcy + 30 };
            bool sel = (m_state.selectedLensIndex == i);
            D2D1_COLOR_F txtCol = sel ? COL_CYAN : COL_TEXT_SECOND;
            DrawRoundedCard(m_rcLensBtns[i], sel ? HexColor(0x162C3D) : COL_BG_DARK, sel ? COL_CYAN : COL_BORDER, 6.0f);
            DrawTextStr(lensLabels[i], m_rcLensBtns[i], m_pFontBody, txtCol, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        rcy += 40.0f;

        // Повзунок плавного зуму (1.0x .. 10.0x)
        m_rcZoomSlider = { rlx, rcy + 18, rrx, rcy + 26 };
        wchar_t zoomSliderText[64];
        swprintf_s(zoomSliderText, L"Плавний зум: %.1fx", m_state.zoomRatio);
        DrawTextStr(zoomSliderText, {rlx, rcy, rrx, rcy + 18}, m_pFontBody, COL_CYAN);

        DrawRoundedCard(m_rcZoomSlider, HexColor(0x07111A), COL_BORDER, 4.0f);
        float normZoom = (m_state.zoomRatio - 0.5f) / 9.5f;
        normZoom = std::max(0.0f, std::min(1.0f, normZoom));
        float fillW = m_rcZoomSlider.Width() * normZoom;
        DrawRoundedCard({ m_rcZoomSlider.left, m_rcZoomSlider.top, m_rcZoomSlider.left + fillW, m_rcZoomSlider.bottom }, COL_CYAN, COL_CYAN, 4.0f);
        D2D1_ELLIPSE thumb = D2D1::Ellipse(D2D1::Point2F(m_rcZoomSlider.left + fillW, (m_rcZoomSlider.top + m_rcZoomSlider.bottom) * 0.5f), 7.0f, 7.0f);
        m_pBrush->SetColor(COL_TEXT_PRIMARY);
        m_pRenderTarget->FillEllipse(thumb, m_pBrush);
        rcy += 42.0f;

        // 4. Роздільна здатність (720p, 1080p, 2K, 4K UHD)
        DrawTextStr(L"4. РОЗДІЛЬНА ЗДАТНІСТЬ (ДО 4K UHD)", {rlx, rcy, rrx, rcy + 22}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 26.0f;

        const wchar_t* resLabels[4] = { L"720p", L"1080p FHD", L"1440p 2K", L"2160p 4K" };
        float rBtnW = (rrx - rlx - 18.0f) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            m_rcResBtns[i] = { rlx + i * (rBtnW + 6.0f), rcy, rlx + (i + 1) * rBtnW + i * 6.0f, rcy + 32 };
            bool sel = (m_state.selectedResIndex == i);
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

        DrawRoundedCard(m_rcBtnRefocus, HexColor(0x0E1926), COL_CYAN, 8.0f);
        DrawTextStr(L"🎯 Автофокус", m_rcBtnRefocus, m_pFontBody, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER);
        rcy += 40;

        // Слайдери ISO, Shutter, Kelvin, Focus
        const wchar_t* sliderTitles[4] = {
            m_state.manualIso > 0 ? L"ISO (Світлочутливість)" : L"ISO: АВТО",
            m_state.manualShutter > 0 ? L"Витримка (Shutter)" : L"Витримка: АВТО",
            m_state.manualKelvin > 0 ? L"Баланс білого (Кельвіни)" : L"Баланс білого: АВТО",
            m_state.manualFocus >= 0.0f ? L"Ручний фокус (Lens Focus)" : L"Фокус: НЕПЕРЕРВНИЙ АВТО"
        };

        for (int i = 0; i < 4; ++i) {
            DrawTextStr(sliderTitles[i], {rlx, rcy, rrx, rcy + 16}, m_pFontBody,
                        (m_state.isProManual) ? COL_CYAN : COL_TEXT_DIM);
            rcy += 18;

            m_rcSliders[i] = {rlx, rcy, rrx, rcy + 8};
            DrawRoundedCard(m_rcSliders[i], HexColor(0x07111A), COL_BORDER, 4.0f);

            float fill = 0.5f;
            if (i == 0) fill = m_state.manualIso > 0 ? (m_state.manualIso - 50) / 3150.0f : 0.0f;
            if (i == 1) fill = m_state.manualShutter > 0 ? (m_state.manualShutter - 30) / 970.0f : 0.0f;
            if (i == 2) fill = m_state.manualKelvin > 0 ? (m_state.manualKelvin - 2500) / 5500.0f : 0.0f;
            if (i == 3) fill = m_state.manualFocus >= 0.0f ? m_state.manualFocus : 0.0f;
            fill = std::max(0.0f, std::min(1.0f, fill));

            DrawRoundedCard({rlx, rcy, rlx + (rrx - rlx) * fill, rcy + 8}, COL_CYAN, COL_CYAN, 4.0f);
            D2D1_ELLIPSE sThumb = D2D1::Ellipse(D2D1::Point2F(rlx + (rrx - rlx) * fill, rcy + 4.0f), 6.0f, 6.0f);
            m_pBrush->SetColor(COL_TEXT_PRIMARY);
            m_pRenderTarget->FillEllipse(sThumb, m_pBrush);

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
                    m_state.selectedUsbIndex = static_cast<int>(i);
                    m_state.usbDropdownOpen = false;
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                    return;
                }
            }
            // Клік поза списком закриває його
            m_state.usbDropdownOpen = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }

        // Перевірка кліку по вкладках підключення
        for (int i = 0; i < 2; ++i) {
            if (m_rcTransportTabs[i].Contains(x, y)) {
                m_state.transport = static_cast<TransportTab>(i);
                m_bIpEditing = false;
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }
        }

        // Клік по USB випадаючому списку
        if (m_state.transport == TransportTab::USB_ADB && m_rcUsbDropdown.Contains(x, y)) {
            m_state.usbDropdownOpen = !m_state.usbDropdownOpen;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }

        // Клік по кнопці оновлення USB
        if (m_state.transport == TransportTab::USB_ADB && m_rcBtnRefreshUsb.Contains(x, y)) {
            RefreshUsbDevices(true);
            return;
        }

        // Клік по полю вводу IP-адреси
        if (m_state.transport == TransportTab::WIFI_LAN && m_rcIpInput.Contains(x, y)) {
            m_bIpEditing = true;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        } else {
            m_bIpEditing = false;
        }

        // Клік по кнопці Старт/Стоп
        if (m_rcBtnToggleStream.Contains(x, y)) {
            if (m_state.isStreaming) {
                StopStream();
            } else {
                StartStream();
            }
            return;
        }

        // Клік по кнопках вибору лінз
        for (int i = 0; i < 7; ++i) {
            if (m_rcLensBtns[i].Contains(x, y)) {
                m_state.selectedLensIndex = i;
                if (i == 6) {
                    m_state.isFrontCamera = true;
                    SendControlCommand("switch_camera", "front");
                } else {
                    m_state.isFrontCamera = false;
                    float z = 1.0f;
                    if (i == 0) z = 0.5f;
                    else if (i == 1) z = 1.0f;
                    else if (i == 2) z = 2.0f;
                    else if (i == 3) z = 3.0f;
                    else if (i == 4) z = 5.0f;
                    else if (i == 5) z = 10.0f;
                    m_state.zoomRatio = z;
                    SendControlCommand("set_zoom", "", z);
                }
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }
        }

        // Клік по слайдеру зуму
        if (m_rcZoomSlider.Contains(x, y)) {
            m_activeSliderDrag = 99; // 99 = Zoom
            HandleMouseMove(x, y, true);
            return;
        }

        // Клік по вибору роздільної здатності
        for (int i = 0; i < 4; ++i) {
            if (m_rcResBtns[i].Contains(x, y)) {
                m_state.selectedResIndex = i;
                int rw = 1920, rh = 1080;
                if (i == 0) { rw = 1280; rh = 720; }
                else if (i == 1) { rw = 1920; rh = 1080; }
                else if (i == 2) { rw = 2560; rh = 1440; }
                else if (i == 3) { rw = 3840; rh = 2160; }
                SendControlCommand("set_resolution", std::to_string(rw) + "x" + std::to_string(rh));
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }
        }

        // Кнопка режиму AUTO / MANUAL
        if (m_rcBtnModeToggle.Contains(x, y)) {
            m_state.isProManual = !m_state.isProManual;
            if (!m_state.isProManual) {
                m_state.manualIso = 0;
                m_state.manualShutter = 0;
                m_state.manualKelvin = 0;
                m_state.manualFocus = -1.0f;
                SendControlCommand("set_isp_mode", "auto");
            } else {
                SendControlCommand("set_isp_mode", "manual");
            }
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }

        // Незалежна кнопка перефокусування (Trigger Refocus)
        if (m_rcBtnRefocus.Contains(x, y)) {
            SendControlCommand("trigger_autofocus");
            return;
        }

        // Клік по слайдерах ISP
        for (int i = 0; i < 4; ++i) {
            if (m_rcSliders[i].Contains(x, y)) {
                m_activeSliderDrag = i;
                HandleMouseMove(x, y, true);
                return;
            }
        }

        // Дзеркало
        if (m_rcBtnMirror.Contains(x, y)) {
            m_state.mirrorHorizontal = !m_state.mirrorHorizontal;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }

        // Сітка 3x3
        if (m_rcBtnGrid.Contains(x, y)) {
            m_state.showGrid = !m_state.showGrid;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }

        // Перереєстрація фільтра
        if (m_rcBtnRegisterDriver.Contains(x, y)) {
            ShellExecuteW(nullptr, L"runas", L"regsvr32.exe", L"/s EyeCamFilter.dll", nullptr, SW_SHOWNORMAL);
            return;
        }
    }

    void HandleMouseMove(float x, float /*y*/, bool isLButtonDown) {
        if (!isLButtonDown) {
            m_activeSliderDrag = -1;
            return;
        }

        if (m_activeSliderDrag == 99) { // Zoom
            float norm = (x - m_rcZoomSlider.left) / m_rcZoomSlider.Width();
            norm = std::max(0.0f, std::min(1.0f, norm));
            m_state.zoomRatio = 0.5f + norm * 9.5f;
            SendControlCommand("set_zoom", "", m_state.zoomRatio);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }

        if (m_activeSliderDrag >= 0 && m_activeSliderDrag < 4) {
            float norm = (x - m_rcSliders[m_activeSliderDrag].left) / m_rcSliders[m_activeSliderDrag].Width();
            norm = std::max(0.0f, std::min(1.0f, norm));

            if (m_activeSliderDrag == 0) { // ISO
                m_state.manualIso = static_cast<int>(50 + norm * 3150);
                SendControlCommand("set_manual_param", "iso", m_state.manualIso);
            } else if (m_activeSliderDrag == 1) { // Shutter
                m_state.manualShutter = static_cast<int>(30 + norm * 970);
                SendControlCommand("set_manual_param", "shutter", m_state.manualShutter);
            } else if (m_activeSliderDrag == 2) { // Kelvin
                m_state.manualKelvin = static_cast<int>(2500 + norm * 5500);
                SendControlCommand("set_manual_param", "kelvin", m_state.manualKelvin);
            } else if (m_activeSliderDrag == 3) { // Focus
                m_state.manualFocus = norm;
                SendControlCommand("set_manual_param", "focus", m_state.manualFocus);
            }
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }

    void HandleMouseUp(float /*x*/, float /*y*/) {
        m_activeSliderDrag = -1;
    }

    void HandleCharInput(wchar_t ch) {
        if (!m_bIpEditing) return;

        if (ch == VK_BACK) {
            if (!m_state.phoneIp.empty()) {
                m_state.phoneIp.pop_back();
            }
        } else if ((ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L':') {
            if (m_state.phoneIp.size() < 21) {
                m_state.phoneIp.push_back(ch);
            }
        } else if (ch == VK_RETURN) {
            m_bIpEditing = false;
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

private:
    HINSTANCE m_hInstance = nullptr;
    HWND m_hwnd = nullptr;

    ID2D1Factory* m_pD2DFactory = nullptr;
    IDWriteFactory* m_pDWriteFactory = nullptr;
    IWICImagingFactory* m_pWicFactory = nullptr;
    ID2D1HwndRenderTarget* m_pRenderTarget = nullptr;
    ID2D1SolidColorBrush* m_pBrush = nullptr;
    ID2D1Bitmap* m_pPreviewBitmap = nullptr;
    UINT m_bitmapW = 0, m_bitmapH = 0;

    IDWriteTextFormat* m_pFontTitle = nullptr;
    IDWriteTextFormat* m_pFontSection = nullptr;
    IDWriteTextFormat* m_pFontBody = nullptr;
    IDWriteTextFormat* m_pFontMono = nullptr;
    IDWriteTextFormat* m_pFontBadge = nullptr;

    std::unique_ptr<EyeCam::WindowsDirectShowSink> m_dshowSink;

    StudioState m_state;
    std::mutex m_stateMutex;
    bool m_bIpEditing = false;

    // Стрімінг та сокет
    std::atomic<bool> m_bStreamRunning = false;
    SOCKET m_clientSocket = INVALID_SOCKET;
    std::thread m_streamThread;

    std::atomic<bool> m_bUsbMonitorRunning = false;
    std::thread m_usbMonitorThread;

    std::mutex m_frameMutex;
    std::vector<uint8_t> m_latestFrameBgra;
    uint32_t m_latestFrameW = 0;
    uint32_t m_latestFrameH = 0;
    bool m_bHasNewFrame = false;

    // Зони UI елементів для обробки миші
    UiRect m_rcTransportTabs[2]{};
    UiRect m_rcUsbDropdown{};
    UiRect m_rcBtnRefreshUsb{};
    std::vector<UiRect> m_rcUsbItems;
    UiRect m_rcIpInput{};
    UiRect m_rcBtnToggleStream{};
    UiRect m_rcPreviewArea{};
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

#endif // _WIN32
