# Архітектурна специфікація EyeCam Studio (C++20 / Qt 6 QML / RHI)

## 1. Загальний огляд системи

**EyeCam Studio** — це високопродуктивний десктопний додаток для перетворення мобільних пристроїв та зовнішніх камер у віртуальні веб-камери студійного рівня з ультра-низькою затримкою (Sub-Frame Latency < 15 мс) та прямим апаратним прискоренням декодування.

Система реалізована за стандартом **C++20** із використанням фреймворку **Qt 6 (Qt Quick / RHI - Rendering Hardware Interface)** та модульної мікроядерної архітектури відеотракту (GStreamer / FFmpeg HW Acceleration).

---

## 2. Архітектура та реактивний менеджмент стану

```mermaid
flowchart TD
    subgraph UI ["Шар інтерфейсу (Qt 6 QML / Glassmorphism)"]
        LS["LeftSidebar: Менеджер пристроїв & Телеметрія"]
        CV["CenterViewport: Відеов'юпорт (Flip H/V, 90° Rot)"]
        RI["RightInspector: Auto / Pro Слайдери (ISO, Shutter, Focus)"]
    end

    subgraph Core ["Ядро стану & Сервіси (C++20 AppState)"]
        State["AppState (Q_PROPERTY / Реактивні зв'язки)"]
        Telem["TelemetryService (EMA фільтр: RTT, FPS, Loss)"]
        Discovery["DeviceDiscoveryService (mDNS Wi-Fi & ADB Poller)"]
    end

    subgraph Pipeline ["Апаратно-прискорений відеотракт"]
        Dec["HW Decoder (NVDEC / D3D11VA / VA-API)"]
        Tee{"Паралельний спліттер (Tee Node)"}
        RHI["Qt RHI Texture Provider (D3D11 Texture Shared Resource)"]
        VCam["VirtualCameraSink (IPC Shared Memory + Named Event)"]
    end

    subgraph Drivers ["Зовнішні споживачі"]
        OSApp["Zoom / OBS / Teams (Stage 3 Virtual Camera)"]
    end

    Discovery -->|Оновлення статусу USB/Wi-Fi| State
    Telem -->|Згладжена метрика (200мс)| State
    State <-->|Двосторонній Data-Binding| UI
    State -->|Команди управління потоком| Dec

    Dec --> Tee
    Tee -->|Гілка А: Zero-Copy UI Рендер| RHI --> CV
    Tee -->|Гілка Б: Безблокувальна потрійна буферизація| VCam --> OSApp
    Tee -.->|Лічильники кадрів & байтів| Telem
```

### Принципи реактивного стану
1. **Єдине джерело правди (`AppState`)**: Стан параметрів камери (ISO, витримка, фокус, WB, відтінок, компенсація експозиції), геометрії (віддзеркалення, кут обертання) та телеметрії ізольовані в `AppState`.
2. **Атомарність та Thread-Safety**: Параметри телеметрії збираються у високошвидкісному потоці відеотракту за допомогою `std::atomic` без використання блокуючих м'ютексів.
3. **Згладжування метрик (Exponential Moving Average)**: Для уникнення візуального тремтіння (jitter) значень FPS, RTT та бітрейту використовується EMA-фільтр з коефіцієнтом $\alpha = 0.3$:
   $$S_t = \alpha \cdot Y_t + (1 - \alpha) \cdot S_{t-1}$$

---

## 3. Апаратно-прискорений відеотракт (Video Pipeline)

### Схема декодування та одночасної роздачі (Dual-Branch Routing)
Відеотракт реалізує архітектуру **Tee-розгалужувача** без повторного кодування (Zero Re-Encoding Overhead):

1. **Апаратний інжест**:
   - **Windows**: Декодування H.264/HEVC через Direct3D 11 Video Acceleration (`D3D11VA`) або NVIDIA Video Codec SDK (`NVDEC`). Декодовані поверхні залишаються у відеопам'яті GPU (`DXGI_FORMAT_NV12`).
   - **Linux**: Декодування через Video Acceleration API (`VA-API`) або `NVDEC` з виділенням поверхні `VASurfaceID`.

2. **Гілка А — Локальний в'юпорт (UI Preview)**:
   - Декодований дескриптор текстури передається напряму в пайплайн **Qt RHI (Direct3D 11 / OpenGL)** через механізм `ID3D11Device::OpenSharedResource` або EGLImage.
   - Трансформації (Flip Horizontal, Flip Vertical, поворот на 90°/180°/270°) виконуються виключно у вершинному шейдері або матриці трансформації QML (`Item.transform`), що гарантує 0 мс накладних витрат CPU.

3. **Гілка Б — Драйвер віртуальної камери Stage 3 (`VirtualCameraSink`)**:
   - Передача кадрів у драйвер віртуальної камери (DirectShow Source Filter або Media Foundation Virtual Camera на Windows, `v4l2loopback` на Linux).
   - Міжпроцесна взаємодія (IPC) базується на **Shared Memory (`CreateFileMappingW` / `shm_open`)** з потрійною кільцевою буферизацією (Triple Buffering):
     - Буфер 0: Читається клієнтським додатком (Zoom/OBS).
     - Буфер 1: Останній повністю готовий актуальний кадр.
     - Буфер 2: Записується відеотрактом EyeCam у поточний момент.
   - Сповіщення драйвера здійснюється за допомогою іменованої події ядра Windows `SetEvent` (`Global\EyeCam_VirtualCamera_Event`), забезпечуючи затримку передачі між процесами менше 0.2 мс.

### Еквівалентний GStreamer конвеєр (Broadcast Reference)
```bash
# Windows Direct3D11 Hardware Pipeline
udpsrc port=8554 caps="application/x-rtp, media=video, encoding-name=H264" ! \
    rtph264depay ! h264parse ! d3d11h264dec ! tee name=t \
    t. ! queue max-size-buffers=1 leaky=downstream ! d3d11convert ! qmlglsink name=local_preview \
    t. ! queue max-size-buffers=1 leaky=downstream ! d3d11download ! video/x-raw,format=NV12 ! \
    shmsink socket-path="Global\\EyeCam_VirtualCamera_Shm" sync=false
```

---

## 4. Специфікація інтерфейсу користувача (UI/UX)

- **Палітра**: Deep Minimalist Dark (`#090B0E`, `#11141A`, `#181D26`).
- **Скляний морфізм (Glassmorphism)**: Напівпрозорі картки (`rgba(20, 25, 35, 0.65)`), градієнтні межі (`1px solid rgba(255, 255, 255, 0.08)`), неонові акценти Cyan (`#00F0FF`) та Cobalt (`#3B82F6`).
- **Компонування**:
  1. **Ліва панель (320px)**: 
     - Менеджер підключень: виявлення Wi-Fi (mDNS/Bonjour) з відображенням рівня сигналу (dBm).
     - USB-канал через фоновий ADB Polling з автоматичним port forwarding (`adb forward tcp:8554 tcp:8554`).
     - Жива телеметрія: RTT (мс), Render FPS, H.264 Бітрейт (Mbps), Відсоток втрати пакетів (%).
  2. **Центральний в'юпорт**:
     - Апаратне масштабування зі збереженням пропорцій (Fit / Fill).
     - Елементи швидкого керування орієнтацією: Flip H, Flip V, поворот на 90°, сітка правила третин.
     - Індикатор розширення (4K UHD 60FPS NV12) та статусу Virtual Camera Sink.
  3. **Правий інспектор (340px)**:
     - Селектор режимів **Auto / Pro**.
     - У режимі Pro — плавні слайдери з неоновим треком та цифровими індикаторами:
       * **ISO**: 100 — 12800.
       * **Shutter Speed**: 1/24 с — 1/8000 с.
       * **Manual Focus**: 0.00 (Macro) — 1.00 (Infinity).
       * **Smooth Zoom**: 1.0x — 10.0x.
       * **White Balance**: 2000K — 10000K + пресети (3200K, 4000K, 5600K, 7000K) + Tint (-50...+50).
