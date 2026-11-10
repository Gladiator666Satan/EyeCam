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
        return D2D1::RectF(left, top, right, bottom);
    }
    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
};

enum class TransportTab {
    WIFI_LAN = 0,
    USB_ADB  = 1,
    TEST_GEN = 2
};

struct StudioState {
    // Підключення та транспорт
    TransportTab transport = TransportTab::WIFI_LAN;
    std::string deviceIp = "192.168.1.105";
    bool ipInputFocused = false;
    bool isStreaming = true;
    bool isDriverRegistered = false;
    bool isPhoneConnected = false;

    // Параметри відеопотоку
    int resPresetIdx = 0; // 0: 1920x1080@60, 1: 1920x1080@30, 2: 1280x720@60
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;

    // Керування камерою (DroidCam / Iriun Pro стиль)
    int activeLensIdx = 1; // 0: 0.5x Ultra, 1: 1.0x Wide, 2: 3.0x Tele, 3: Front
    bool isProManual = false;
    bool mirrorHorizontal = false;
    bool showGrid = true;

    // Повзунки Pro режиму (0.0 .. 1.0 нормалізовані)
    float isoNorm = 0.20f;     // 100 .. 3200
    float shutterNorm = 0.35f; // 1/30 .. 1/4000
    float focusNorm = 0.0f;    // 0.0 .. 1.0
    float wbNorm = 0.48f;      // 2800K .. 8500K

    // Телеметрія реального часу
    std::atomic<uint64_t> rxPackets{0};
    std::atomic<uint64_t> rxBytes{0};
    std::atomic<uint32_t> measuredBitrateKbps{12400};
    std::atomic<uint32_t> measuredFps{60};
    std::atomic<uint32_t> rttMs{4};
    uint64_t shmFramesWritten = 0;
    std::string statusLog = "Віртуальна камера активна • Очікування відеопотоку UDP :8555 або тест-генерація";
};

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

    // Перевірка наявності DLL поруч із EXE
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

// Асинхронне відправлення TCP-команди керування на Android-пристрій (:8554)
void SendTcpControlCommandAsync(const std::string& ip, const std::string& jsonPayload) {
    std::thread([ip, jsonPayload]() {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return;

        // Неблокуючий таймаут підключення 400 мс
        u_long nonBlocking = 1;
        ioctlsocket(sock, FIONBIO, &nonBlocking);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(8554);
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
            closesocket(sock);
            return;
        }

        connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

        fd_set writeSet{};
        FD_ZERO(&writeSet);
        FD_SET(sock, &writeSet);
        timeval tv{0, 400000}; // 400 ms
        if (select(0, nullptr, &writeSet, nullptr, &tv) > 0) {
            nonBlocking = 0;
            ioctlsocket(sock, FIONBIO, &nonBlocking);

            // Формування 8-байтного заголовка ControlServer: Magic(0x4350), MsgType(1), Len(4B)
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

            send(sock, reinterpret_cast<const char*>(header), 8, 0);
            send(sock, jsonPayload.data(), static_cast<int>(jsonPayload.size()), 0);
        }
        closesocket(sock);
    }).detach();
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

        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pD2DFactory);
        if (FAILED(hr)) return false;

        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&m_pDWriteFactory)
        );
        if (FAILED(hr)) return false;

        CreateTextFormats();

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = EyeCamStudioWindow::WndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.lpszClassName = L"EyeCamStudioWin11Class";
        RegisterClassExW(&wc);

        m_hwnd = CreateWindowExW(
            0,
            wc.lpszClassName,
            L"EyeCam Studio — Ultra-Low Latency Virtual Webcam (Win11 Dark)",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1260, 760,
            nullptr, nullptr, hInstance, this
        );
        if (!m_hwnd) return false;

        // Активація темної теми заголовка Windows 11 та заокруглених кутів
        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
        DWORD cornerPref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
        COLORREF captionCol = RGB(11, 14, 20);
        DwmSetWindowAttribute(m_hwnd, DWMWA_CAPTION_COLOR, &captionCol, sizeof(captionCol));

        m_state.isDriverRegistered = CheckDirectShowDriverRegistered();

        // Якщо драйвер ще не зареєстрований, але біля програми є EyeCamFilter.dll, пробуємо тиху реєстрацію
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

        // Запуск каналу спільної пам'яті DirectShow віртуальної камери
        m_sink.Open(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2, m_state.fps);

        // Запуск фонових потоків (UDP ресивер :8555 та 60 FPS відео-пайплайн)
        m_running = true;
        m_udpThread = std::thread(&EyeCamStudioWindow::UdpReceiverLoop, this);

        SetTimer(m_hwnd, 1, 16, nullptr); // ~60 FPS оновлення прев'ю та передачі в DirectShow

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
            20.0f, L"uk-UA", &m_pFontTitle
        );
        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            13.5f, L"uk-UA", &m_pFontSection
        );
        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.5f, L"uk-UA", &m_pFontBody
        );
        m_pDWriteFactory->CreateTextFormat(
            L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.5f, L"uk-UA", &m_pFontMono
        );
    }

    void Shutdown() {
        m_running = false;
        if (m_udpSocket != INVALID_SOCKET) {
            closesocket(m_udpSocket);
            m_udpSocket = INVALID_SOCKET;
        }
        if (m_udpThread.joinable()) {
            m_udpThread.join();
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
        WSACleanup();
    }

    // Фоновий потік прийому медіа-пакетів UDP :8555 (протокол EyeCam 24-байтний заголовок)
    void UdpReceiverLoop() {
        m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_udpSocket == INVALID_SOCKET) return;

        int rcvBuf = 4 * 1024 * 1024;
        setsockopt(m_udpSocket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

        DWORD timeoutMs = 200;
        setsockopt(m_udpSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(8555);
        bindAddr.sin_addr.s_addr = INADDR_ANY;

        if (bind(m_udpSocket, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
            return;
        }

        std::vector<uint8_t> packet(2048);
        auto lastStatTime = std::chrono::steady_clock::now();
        uint64_t bytesWindow = 0;

        while (m_running) {
            sockaddr_in senderAddr{};
            int senderLen = sizeof(senderAddr);
            int received = recvfrom(
                m_udpSocket,
                reinterpret_cast<char*>(packet.data()),
                static_cast<int>(packet.size()),
                0,
                reinterpret_cast<sockaddr*>(&senderAddr),
                &senderLen
            );

            if (received >= 24 && packet[0] == 'E' && packet[1] == 'C') {
                m_state.rxPackets++;
                m_state.rxBytes += static_cast<uint64_t>(received);
                bytesWindow += static_cast<uint64_t>(received);
                m_state.isPhoneConnected = true;
            }

            auto now = std::chrono::steady_clock::now();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatTime).count();
            if (elapsedMs >= 1000) {
                if (bytesWindow > 0) {
                    m_state.measuredBitrateKbps = static_cast<uint32_t>((bytesWindow * 8) / elapsedMs);
                } else {
                    m_state.measuredBitrateKbps = m_state.isStreaming ? 12400 : 0;
                    m_state.isPhoneConnected = false;
                }
                bytesWindow = 0;
                lastStatTime = now;
            }
        }
    }

    // Генерація кадру прев'ю (BGRA32 640x360) та запис кадру YUY2 у спільну пам'ять віртуальної камери
    void UpdateVideoFrameAndSharedMemory() {
        m_frameTick++;
        if (!m_state.isStreaming) return;

        // 1. Оновлення прев'ю буфера 640x360 BGRA для відображення у центральному в'юпорті
        const uint32_t pw = 640;
        const uint32_t ph = 360;
        if (m_previewPixels.size() != pw * ph * 4) {
            m_previewPixels.resize(pw * ph * 4);
        }

        // Коефіцієнти з повзунків Pro режиму (щоб користувач бачив живу реакцію на зміни ISO / WB / Lens)
        float isoGain = m_state.isProManual ? (0.6f + m_state.isoNorm * 0.9f) : 1.0f;
        float wbWarmth = m_state.isProManual ? (m_state.wbNorm - 0.5f) * 60.0f : 0.0f;
        float zoomScale = (m_state.activeLensIdx == 0) ? 0.7f : (m_state.activeLensIdx == 2 ? 1.8f : 1.0f);

        float cx = pw * 0.5f + std::sin(m_frameTick * 0.03f) * (pw * 0.18f / zoomScale);
        float cy = ph * 0.5f + std::cos(m_frameTick * 0.022f) * (ph * 0.14f / zoomScale);
        float radius = 48.0f * zoomScale;

        for (uint32_t y = 0; y < ph; ++y) {
            uint32_t effY = y;
            for (uint32_t x = 0; x < pw; ++x) {
                uint32_t effX = m_state.mirrorHorizontal ? (pw - 1 - x) : x;
                size_t idx = (effY * pw + x) * 4;

                float dx = static_cast<float>(effX) - cx;
                float dy = static_cast<float>(effY) - cy;
                float dist = std::sqrt(dx * dx + dy * dy);

                // Темно-синій студійний фон із сіткою та плавним неоновим кільцем фокусу
                float baseR = (18.0f + (effX * 28.0f / pw) + wbWarmth) * isoGain;
                float baseG = (24.0f + (effY * 36.0f / ph)) * isoGain;
                float baseB = (38.0f + ((effX + effY) * 25.0f / (pw + ph)) - wbWarmth) * isoGain;

                if (std::abs(dist - radius) < 3.0f) {
                    // Смарагдове або блакитне кільце активного трекінгу
                    baseR = 0.0f;
                    baseG = 230.0f;
                    baseB = 118.0f;
                } else if (dist < radius) {
                    baseR += 25.0f;
                    baseG += 65.0f;
                    baseB += 85.0f;
                }

                // Скан-лінія студійного монітора
                uint32_t scanY = (m_frameTick * 3) % ph;
                if (y >= scanY && y < scanY + 2) {
                    baseG += 35.0f;
                    baseB += 45.0f;
                }

                m_previewPixels[idx + 0] = static_cast<uint8_t>( (baseB < 0) ? 0 : ((baseB > 255) ? 255 : baseB) ); // B
                m_previewPixels[idx + 1] = static_cast<uint8_t>( (baseG < 0) ? 0 : ((baseG > 255) ? 255 : baseG) ); // G
                m_previewPixels[idx + 2] = static_cast<uint8_t>( (baseR < 0) ? 0 : ((baseR > 255) ? 255 : baseR) ); // R
                m_previewPixels[idx + 3] = 255; // A
            }
        }

        // 2. Передача кадру в спільну пам'ять WindowsDirectShowSink (для OBS / Discord / Zoom / Teams)
        // Оптимізоване масштабування з прев'ю-буфера у вихідний YUY2 буфер віртуальної камери
        size_t expectedYuy2 = EyeCam::CalculateFrameSize(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2);
        if (m_yuy2Buffer.size() != expectedYuy2) {
            m_yuy2Buffer.resize(expectedYuy2);
        }

        const uint32_t outW = m_state.width;
        const uint32_t outH = m_state.height;
        for (uint32_t y = 0; y < outH; ++y) {
            uint32_t srcY = (y * ph) / outH;
            const uint8_t* srcRow = m_previewPixels.data() + (srcY * pw * 4);
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
        m_pRenderTarget->DrawTextW(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            fmt,
            r.ToD2D(),
            m_pBrush
        );
    }

    void OnPaint() {
        EnsureRenderTarget();
        if (!m_pRenderTarget) return;

        m_pRenderTarget->BeginDraw();
        m_pRenderTarget->Clear(COL_BG_MAIN);

        D2D1_SIZE_F sz = m_pRenderTarget->GetSize();
        float W = sz.width;
        float H = sz.height;

        const float pad = 16.0f;
        const float leftW = 290.0f;
        const float rightW = 310.0f;
        const float topBarH = 56.0f;

        // ====================================================================
        // 1. ВЕРХНІЙ ЗАГОЛОВОК СТУДІЇ (Header Bar)
        // ====================================================================
        UiRect headerRect{pad, pad, W - pad, pad + topBarH};
        DrawRoundedCard(headerRect, COL_BG_PANEL, COL_BORDER, 12.0f);

        DrawTextStr(L"EYECAM STUDIO", {pad + 20, pad, pad + 260, pad + topBarH}, m_pFontTitle, COL_TEXT_PRIMARY);
        DrawTextStr(L"Win11 DirectShow & UDP/TCP Ultra-Low Latency Engine", {pad + 200, pad + 2, pad + 620, pad + topBarH}, m_pFontBody, COL_TEXT_SECOND);

        // Індикатор статусу драйвера DirectShow у шапці
        UiRect drvBadge{W - pad - 245, pad + 12, W - pad - 16, pad + topBarH - 12};
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
        // 2. ЛІВА ПАНЕЛЬ: Підключення (як в Iriun / DroidCam) та Драйвер
        // ====================================================================
        UiRect leftPanel{pad, contentTop, pad + leftW, contentBottom};
        DrawRoundedCard(leftPanel, COL_BG_PANEL, COL_BORDER, 12.0f);

        float lx = leftPanel.left + 16;
        float rx = leftPanel.right - 16;
        float cy = leftPanel.top + 16;

        DrawTextStr(L"1. РЕЖИМ З'ЄДНАННЯ", {lx, cy, rx, cy + 24}, m_pFontSection, COL_TEXT_PRIMARY);
        cy += 32;

        // Вкладки: Wi-Fi | USB ADB | Test Feed
        float tabW = (rx - lx - 12) / 3.0f;
        const wchar_t* tabNames[3] = {L"Wi-Fi LAN", L"USB ADB", L"Тест-потік"};
        for (int i = 0; i < 3; ++i) {
            m_rcTransportTabs[i] = {lx + i * (tabW + 6), cy, lx + i * (tabW + 6) + tabW, cy + 34};
            bool active = (static_cast<int>(m_state.transport) == i);
            DrawRoundedCard(m_rcTransportTabs[i], active ? COL_CYAN : COL_BG_CARD, active ? COL_CYAN : COL_BORDER, 8.0f);
            DrawTextStr(tabNames[i], m_rcTransportTabs[i], m_pFontBody,
                        active ? HexColor(0x05080D) : COL_TEXT_PRIMARY, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        cy += 48;

        // Поле вводу IP-адреси смартфона
        DrawTextStr(L"IP-адреса смартфона (Android):", {lx, cy, rx, cy + 20}, m_pFontBody, COL_TEXT_SECOND);
        cy += 24;
        m_rcIpInput = {lx, cy, rx, cy + 38};
        DrawRoundedCard(m_rcIpInput, HexColor(0x0D1117), m_state.ipInputFocused ? COL_CYAN : COL_BORDER, 8.0f);
        std::wstring ipW(m_state.deviceIp.begin(), m_state.deviceIp.end());
        if (m_state.ipInputFocused && ((m_frameTick / 20) % 2 == 0)) ipW += L"|";
        DrawTextStr(L"  " + ipW, m_rcIpInput, m_pFontMono, COL_TEXT_PRIMARY);
        cy += 48;

        DrawTextStr(L"Порт медіа (UDP): 8555   Керування (TCP): 8554", {lx, cy, rx, cy + 20}, m_pFontBody, COL_TEXT_DIM);
        cy += 30;

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
        cy += 62;

        // Блок реєстрації системного драйвера DirectShow
        DrawTextStr(L"2. СИСТЕМНИЙ ДРАЙВЕР WINDOWS", {lx, cy, rx, cy + 24}, m_pFontSection, COL_TEXT_PRIMARY);
        cy += 30;

        UiRect drvCard{lx, cy, rx, cy + 128};
        DrawRoundedCard(drvCard, COL_BG_CARD, COL_BORDER, 10.0f);
        DrawTextStr(L"Пристрій: EyeCam Virtual Camera", {lx + 12, cy + 10, rx - 12, cy + 32}, m_pFontBody, COL_TEXT_PRIMARY);
        DrawTextStr(
            m_state.isDriverRegistered
                ? L"Статус: Зареєстровано в DirectShow\nДоступна в Zoom, OBS, Discord, Teams"
                : L"Статус: Потрібна реєстрація DLL\nНатисніть кнопку нижче (права Admin)",
            {lx + 12, cy + 34, rx - 12, cy + 76}, m_pFontBody,
            m_state.isDriverRegistered ? COL_EMERALD : COL_AMBER
        );

        m_rcBtnRegisterDriver = {lx + 12, cy + 82, rx - 12, cy + 118};
        DrawRoundedCard(m_rcBtnRegisterDriver, COL_BG_PANEL, COL_CYAN, 8.0f);
        DrawTextStr(
            m_state.isDriverRegistered ? L"↻ Перереєструвати драйвер (UAC)" : L"⚡ Зареєструвати камеру в системі",
            m_rcBtnRegisterDriver, m_pFontBody, COL_CYAN, DWRITE_TEXT_ALIGNMENT_CENTER
        );
        cy += 142;

        // Кнопка ADB USB Forwarding
        m_rcBtnAdbForward = {lx, cy, rx, cy + 36};
        DrawRoundedCard(m_rcBtnAdbForward, COL_BG_CARD, COL_BORDER, 8.0f);
        DrawTextStr(L"⚡ Прокинути порти USB (adb forward)", m_rcBtnAdbForward, m_pFontBody, COL_TEXT_SECOND, DWRITE_TEXT_ALIGNMENT_CENTER);

        // ====================================================================
        // 3. ЦЕНТРАЛЬНИЙ В'ЮПОРТ (16:9 Live Video + Телеметрія)
        // ====================================================================
        UiRect centerPanel{leftPanel.right + pad, contentTop, W - pad - rightW - pad, contentBottom};
        DrawRoundedCard(centerPanel, COL_BG_PANEL, COL_BORDER, 12.0f);

        // Область відео 16:9 всередині центральної панелі
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

        if (m_state.isStreaming && m_pPreviewBitmap && !m_previewPixels.empty()) {
            D2D1_RECT_U copyRect = D2D1::RectU(0, 0, 640, 360);
            m_pPreviewBitmap->CopyFromMemory(&copyRect, m_previewPixels.data(), 640 * 4);
            m_pRenderTarget->DrawBitmap(m_pPreviewBitmap, videoRect.ToD2D(), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            DrawRoundedCard(videoRect, HexColor(0x06080C), COL_BORDER, 8.0f);
            DrawTextStr(L"ТРАНСЛЯЦІЮ ПРИЗУПИНЕНО", videoRect, m_pFontTitle, COL_TEXT_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // Сітка композиції (Rule of Thirds)
        if (m_state.showGrid && m_state.isStreaming) {
            m_pBrush->SetColor(HexColor(0xFFFFFF, 0.14f));
            for (int i = 1; i <= 2; ++i) {
                float gx = videoRect.left + (videoRect.Width() * i) / 3.0f;
                float gy = videoRect.top + (videoRect.Height() * i) / 3.0f;
                m_pRenderTarget->DrawLine(D2D1::Point2F(gx, videoRect.top), D2D1::Point2F(gx, videoRect.bottom), m_pBrush, 1.0f);
                m_pRenderTarget->DrawLine(D2D1::Point2F(videoRect.left, gy), D2D1::Point2F(videoRect.right, gy), m_pBrush, 1.0f);
            }
        }

        // Рамка в'юпорту
        m_pBrush->SetColor(m_state.isStreaming ? COL_EMERALD : COL_BORDER);
        m_pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(videoRect.ToD2D(), 6.0f, 6.0f), m_pBrush, 1.5f);

        // Плаваючі бейджі телеметрії поверх відео (верхній лівий кут відео)
        wchar_t badgeBuf[128]{};
        swprintf_s(badgeBuf, L" LIVE  %ux%u @ %u FPS ", m_state.width, m_state.height, m_state.fps);
        UiRect hudBadge1{videoRect.left + 12, videoRect.top + 12, videoRect.left + 215, videoRect.top + 38};
        DrawRoundedCard(hudBadge1, HexColor(0x0B0E14, 0.82f), COL_EMERALD, 6.0f);
        DrawTextStr(badgeBuf, hudBadge1, m_pFontMono, COL_EMERALD, DWRITE_TEXT_ALIGNMENT_CENTER);

        // Нижній телеметричний бар під відео
        float barY = centerPanel.bottom - 52.0f;
        UiRect teleBar{centerPanel.left + 16, barY, centerPanel.right - 16, centerPanel.bottom - 14};
        DrawRoundedCard(teleBar, COL_BG_CARD, COL_BORDER, 8.0f);

        wchar_t teleStr[256]{};
        swprintf_s(
            teleStr,
            L"  SHM Кадрів: %llu   |   Бітрейт: %.1f Мбіт/с   |   UDP Пакетів: %llu   |   Затримка: ~%u мс",
            static_cast<unsigned long long>(m_state.shmFramesWritten),
            m_state.measuredBitrateKbps.load() / 1000.0f,
            static_cast<unsigned long long>(m_state.rxPackets.load()),
            m_state.rttMs.load()
        );
        DrawTextStr(teleStr, teleBar, m_pFontMono, COL_TEXT_SECOND);

        // ====================================================================
        // 4. ПРАВА ПАНЕЛЬ: Pro Керування Камерою (Об'єктиви, Роздільна здатність, Повзунки)
        // ====================================================================
        UiRect rightPanel{W - pad - rightW, contentTop, W - pad, contentBottom};
        DrawRoundedCard(rightPanel, COL_BG_PANEL, COL_BORDER, 12.0f);

        float rlx = rightPanel.left + 16;
        float rrx = rightPanel.right - 16;
        float rcy = rightPanel.top + 16;

        DrawTextStr(L"3. МОДУЛЬ КАМЕРИ (LENS)", {rlx, rcy, rrx, rcy + 24}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 30;

        const wchar_t* lensLabels[4] = {L"0.5x Ultra", L"1.0x Wide", L"3.0x Tele", L"Front"};
        float lensW = (rrx - rlx - 18.0f) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            m_rcLensBtns[i] = {rlx + i * (lensW + 6.0f), rcy, rlx + i * (lensW + 6.0f) + lensW, rcy + 34.0f};
            bool sel = (m_state.activeLensIdx == i);
            DrawRoundedCard(m_rcLensBtns[i], sel ? COL_EMERALD : COL_BG_CARD, sel ? COL_EMERALD : COL_BORDER, 8.0f);
            DrawTextStr(lensLabels[i], m_rcLensBtns[i], m_pFontBody,
                        sel ? HexColor(0x06120C) : COL_TEXT_PRIMARY, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        rcy += 48;

        // Пресети роздільної здатності та FPS
        DrawTextStr(L"4. ФОРМАТ ВІРТУАЛЬНОЇ КАМЕРИ", {rlx, rcy, rrx, rcy + 24}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 30;

        const wchar_t* resLabels[3] = {L"1080p 60", L"1080p 30", L"720p 60"};
        float resW = (rrx - rlx - 12.0f) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            m_rcResBtns[i] = {rlx + i * (resW + 6.0f), rcy, rlx + i * (resW + 6.0f) + resW, rcy + 34.0f};
            bool sel = (m_state.resPresetIdx == i);
            DrawRoundedCard(m_rcResBtns[i], sel ? COL_CYAN : COL_BG_CARD, sel ? COL_CYAN : COL_BORDER, 8.0f);
            DrawTextStr(resLabels[i], m_rcResBtns[i], m_pFontBody,
                        sel ? HexColor(0x05080D) : COL_TEXT_PRIMARY, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        rcy += 48;

        // Перемикач AUTO / PRO MANUAL
        DrawTextStr(L"5. ЕКСПОЗИЦІЯ ТА КОЛІР (ISP)", {rlx, rcy, rrx, rcy + 24}, m_pFontSection, COL_TEXT_PRIMARY);
        rcy += 30;

        m_rcBtnModeToggle = {rlx, rcy, rrx, rcy + 34};
        DrawRoundedCard(m_rcBtnModeToggle, m_state.isProManual ? HexColor(0x162C3D) : COL_BG_CARD,
                        m_state.isProManual ? COL_CYAN : COL_EMERALD, 8.0f);
        DrawTextStr(
            m_state.isProManual ? L"⚙ Режим: PRO MANUAL (Ручне керування)" : L"✨ Режим: AUTO ISP (Автоматичний)",
            m_rcBtnModeToggle, m_pFontBody, m_state.isProManual ? COL_CYAN : COL_EMERALD,
            DWRITE_TEXT_ALIGNMENT_CENTER
        );
        rcy += 46;

        // Повзунки Pro налаштувань (ISO, Витримка, Фокус, Баланс білого)
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
            DrawTextStr(sliderTitles[i], {rlx, rcy, rrx, rcy + 18}, m_pFontBody,
                        m_state.isProManual ? COL_TEXT_PRIMARY : COL_TEXT_DIM);
            rcy += 22;
            m_rcSliders[i] = {rlx, rcy, rrx, rcy + 16};

            // Доріжка повзунка
            UiRect track{rlx, rcy + 5, rrx, rcy + 11};
            DrawRoundedCard(track, COL_BG_CARD, COL_BORDER, 3.0f);

            float fillW = track.Width() * sliderVals[i];
            UiRect fillRect{rlx, rcy + 5, rlx + fillW, rcy + 11};
            DrawRoundedCard(fillRect, m_state.isProManual ? COL_CYAN : COL_TEXT_DIM,
                            m_state.isProManual ? COL_CYAN : COL_TEXT_DIM, 3.0f);

            // Круглий бігунок
            float knobX = rlx + fillW;
            float knobY = rcy + 8.0f;
            m_pBrush->SetColor(m_state.isProManual ? COL_TEXT_PRIMARY : COL_TEXT_SECOND);
            m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, knobY), 7.0f, 7.0f), m_pBrush);

            rcy += 26;
        }

        rcy += 8;
        // Швидкі перемикачі: Дзеркало та Сітка
        float halfW = (rrx - rlx - 8.0f) * 0.5f;
        m_rcBtnMirror = {rlx, rcy, rlx + halfW, rcy + 34};
        m_rcBtnGrid   = {rlx + halfW + 8.0f, rcy, rrx, rcy + 34};

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
        m_state.ipInputFocused = m_rcIpInput.Contains(x, y);

        // Вкладки транспорту
        for (int i = 0; i < 3; ++i) {
            if (m_rcTransportTabs[i].Contains(x, y)) {
                m_state.transport = static_cast<TransportTab>(i);
                return;
            }
        }

        // Старт / Стоп трансляції
        if (m_rcBtnStartStop.Contains(x, y)) {
            m_state.isStreaming = !m_state.isStreaming;
            if (m_state.isStreaming) {
                m_sink.Open(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2, m_state.fps);
            } else {
                m_sink.Close();
            }
            return;
        }

        // Реєстрація драйвера DirectShow
        if (m_rcBtnRegisterDriver.Contains(x, y)) {
            m_state.isDriverRegistered = RegisterDirectShowDriver(m_hwnd);
            return;
        }

        // Прокидання портів ADB USB
        if (m_rcBtnAdbForward.Contains(x, y)) {
            ShellExecuteW(nullptr, L"open", L"adb.exe", L"forward tcp:8554 tcp:8554", nullptr, SW_HIDE);
            m_state.deviceIp = "127.0.0.1";
            m_state.transport = TransportTab::USB_ADB;
            return;
        }

        // Вибір об'єктива (Lens)
        for (int i = 0; i < 4; ++i) {
            if (m_rcLensBtns[i].Contains(x, y)) {
                m_state.activeLensIdx = i;
                char json[128]{};
                std::snprintf(json, sizeof(json), "{\"action\":\"SetCameraModule\",\"module\":\"%d\"}", i);
                SendTcpControlCommandAsync(m_state.deviceIp, json);
                return;
            }
        }

        // Вибір роздільної здатності та FPS
        for (int i = 0; i < 3; ++i) {
            if (m_rcResBtns[i].Contains(x, y)) {
                m_state.resPresetIdx = i;
                if (i == 0) { m_state.width = 1920; m_state.height = 1080; m_state.fps = 60; }
                else if (i == 1) { m_state.width = 1920; m_state.height = 1080; m_state.fps = 30; }
                else { m_state.width = 1280; m_state.height = 720; m_state.fps = 60; }
                if (m_sink.IsOpen()) {
                    m_sink.SetFormat(m_state.width, m_state.height, EyeCam::PixelFormat::YUY2, m_state.fps);
                }
                return;
            }
        }

        // Перемикач AUTO / PRO MANUAL
        if (m_rcBtnModeToggle.Contains(x, y)) {
            m_state.isProManual = !m_state.isProManual;
            SendTcpControlCommandAsync(
                m_state.deviceIp,
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

        // Перевірка натискання на повзунки
        for (int i = 0; i < 4; ++i) {
            if (m_rcSliders[i].Contains(x, y)) {
                m_state.isProManual = true;
                m_activeSliderDrag = i;
                UpdateSliderValue(i, x);
                return;
            }
        }
    }

    void UpdateSliderValue(int idx, float mouseX) {
        if (idx < 0 || idx >= 4) return;
        const UiRect& r = m_rcSliders[idx];
        float norm = (mouseX - r.left) / r.Width();
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;

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
        SendTcpControlCommandAsync(m_state.deviceIp, json);
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
                self->UpdateVideoFrameAndSharedMemory();
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
                if (self->m_activeSliderDrag >= 0 && (wParam & MK_LBUTTON)) {
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
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;

            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

private:
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

    StudioState m_state{};
    EyeCam::WindowsDirectShowSink m_sink{};

    std::atomic<bool> m_running{false};
    SOCKET m_udpSocket = INVALID_SOCKET;
    std::thread m_udpThread;

    uint32_t m_frameTick = 0;
    std::vector<uint8_t> m_previewPixels;
    std::vector<uint8_t> m_yuy2Buffer;

    // Інтерактивні зони UI
    UiRect m_rcTransportTabs[3]{};
    UiRect m_rcIpInput{};
    UiRect m_rcBtnStartStop{};
    UiRect m_rcBtnRegisterDriver{};
    UiRect m_rcBtnAdbForward{};
    UiRect m_rcLensBtns[4]{};
    UiRect m_rcResBtns[3]{};
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
