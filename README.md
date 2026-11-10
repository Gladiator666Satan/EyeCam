# EyeCam: High-Performance Ultra-Low-Latency Virtual Webcam Pipeline
## Специфікація Мережевої Архітектури та Протоколу Передачі Даних

Цей документ описує дворівневу мережеву архітектуру для передачі відео наднизької затримки (< 30-50 мс "glass-to-glass") між смартфоном (Android) та комп'ютером (Windows/Linux) для створення віртуальної веб-камери (Virtual Webcam / OBS Direct Source).

---

## 1. Архітектура Подвійного Каналу (Dual-Channel Architecture)

Система розділена на два незалежних мережевих канали з різними вимогами до QoS:

```
+------------------------------------------------------------------------+
|                            ANDROID DEVICE                              |
|  [ Camera2 API ] -> [ MediaCodec (H.264/HEVC) ] -> [ UDP Media Plane ] |
|  [ Camera Control Daemon ] <--------------------> [ TCP Control Plane] |
+------------------------------------------------------------------------+
                               |     |
                  Wi-Fi (UDP)  |     |  Wi-Fi / USB (TCP)
                  USB (RNDIS)  |     |  USB (ADB Forward)
                               v     v
+------------------------------------------------------------------------+
|                            DESKTOP CLIENT                              |
|  [ UDP Media Receiver ] -> [ Low-Latency Jitter Buffer ] -> [ Decoder ] |
|  [ Control Client (Ping / RTT / Commands) ]                             |
|  [ Virtual Webcam Driver (DirectShow / V4L2 Loopback / AkvCam) ]        |
+------------------------------------------------------------------------+
```

### 1.1. Медіа-канал (Media Plane)
- **Транспорт**: **UDP (Unreliable Datagrams)** через Wi-Fi або USB Tethering (RNDIS/CDC-NCM); або Framed TCP fallback при роботі виключно через ADB port forwarding.
- **Payload**: NAL-юніти кодеків **H.264 (AVC)** або **H.265 (HEVC)** (baseline / main profile, zero-latency tune, no B-frames).
- **Фрагментація**: Поділ NALU на фрагменти розміром до 1400 байтів для гарантії проходження крізь стандартний Ethernet MTU (1500 байтів) без IP-фрагментації:
  $$\text{MTU (1500)} - \text{IPv4 Header (20)} - \text{UDP Header (8)} - \text{EyeCam Header (24)} = 1448 \text{ байтів макс.}$$
- **Zero-Retransmission Policy**:
  - Категорична відмова від повторної відправки (ARQ / retransmission) втрачених пакетів живого відео.
  - Повторні запити пакетів в умовах live-streaming створюють явище Head-of-Line Blocking та катастрофічно роздувають затримку (Bufferbloat).
  - При виявленні пропуску пакетів у P-кадрі (порушення лічильника `sequence_num`), весь кадр відкидається для уникнення артефактів декодування ("змазування" зображення).
  - У разі втрати I-кадру (IDR) або тривалої серії втрат Desktop-клієнт миттєво відправляє запит `KeyframeRequestMessage` (PLI / FIR) через Control Plane.

### 1.2. Контрольний канал (Control Plane)
- **Транспорт**: **TCP Stream з увімкненим прапорцем `TCP_NODELAY`** (повна деактивація алгоритму Нейгла для виключення 40-мс затримок накопичення пакетів).
- **Формат повідомлень**: Двійкове обрамлення (8-байтний заголовок `ControlFrame`) + бінарний Protobuf або легковажний мініфікований JSON.
- **Призначення**:
  - Початкове рукостискання (Handshake), узгодження роздільної здатності (1080p, 4K, 720p), FPS (30/60) та вибір кодека.
  - Керування параметрами камери на льоту: вибір сенсора (Main, Ultra-Wide, Telephoto, Front), ручне ISO, витримка (shutter), фокус, баланс білого (Kelvin).
  - Високоточна синхронізація часу та моніторинг RTT / джиттеру через NTP-подібний Ping/Pong.
  - Зворотний зв'язок (Feedback Channel): сигналізація запитів IDR (Picture Loss Indication).

---

## 2. Специфікація Бінарних Протоколів

### 2.1. Медіа-заголовок (Media Packet Header) — 24 байти
Фіксований бінарний заголовок передує кожному UDP-пакету з відеофрагментом:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Magic (0x45 0x43)    |     Flags     |   Codec Type  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|        Sequence Number        |            Frame ID           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Frame ID (cont.)        |         Fragment Index        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         Fragment Total        |                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               +
|                 Presentation Timestamp (PTS, us)              |
|                         (64-bit uint)                         |
+                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                               |         Payload Length        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          NALU Payload...                      |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

#### Опис полів:
| Байт(и) | Поле | Тип | Опис |
| :--- | :--- | :--- | :--- |
| **0..1** | `Magic` | `uint16` (2B) | `0x4543` (ASCII `'EC'`). Швидка валідація пакета EyeCam. |
| **2** | `Flags` | `uint8` (1B) | Бітова маска: `0x01` = KEYFRAME (IDR), `0x02` = START_OF_FRAME, `0x04` = END_OF_FRAME. |
| **3** | `Codec Type` | `uint8` (1B) | `1` = H.264 (AVC), `2` = HEVC (H.265), `3` = AV1. |
| **4..5** | `Sequence Num` | `uint16` (2B) | Наскрізний інкрементний лічильник кожного відправленого UDP-пакета (0..65535). Виявляє випадання пакетів. |
| **6..9** | `Frame ID` | `uint32` (4B) | Порядковий номер відеокадру. Всі фрагменти одного кадру мають однаковий `Frame ID`. |
| **10..11**| `Fragment Index`| `uint16` (2B) | Індекс поточного фрагмента (0, 1, ..., Total-1). |
| **12..13**| `Fragment Total`| `uint16` (2B) | Загальна кількість фрагментів, на які розбито поточний NAL-юніт. |
| **14..21**| `PTS (us)` | `uint64` (8B) | Мітка часу відтворення у мікросекундах для згладжування в jitter buffer. |
| **22..23**| `Payload Length`| `uint16` (2B) | Довжина відеоданих (без урахування 24 байтів заголовка). |

---

### 2.2. Заголовок контрольного каналу (Control Frame Header) — 8 байтів

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Magic (0x43 0x50)    |          Message Type         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Payload Length (uint32)                   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Message Payload Bytes...                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- **Magic**: `0x4350` (ASCII `'CP'`).
- **Message Type**:
  - `1`: JSON Command
  - `2`: Protobuf Binary Message
  - `3`: FramePing ($T_1$)
  - `4`: FramePong ($T_1, T_2, T_3$)
  - `5`: KeyframeRequest (IDR/PLI)
  - `6`: KeepAlive Heartbeat
- **Payload Length**: 4 байти Big-Endian розміру корисного навантаження.

---

## 3. Network Discovery & Transport Strategy

### 3.1. Wi-Fi: mDNS / ZeroConf Автоматичне Виявлення
- Сервіс транслюється Android-додатком за допомогою `android.net.nsd.NsdManager`.
- **Тип сервісу**: `_eyecam-control._tcp.local.`
- **Порт за замовчуванням**: `8554`
- **TXT Записи**:
  - `v=1`
  - `name=Pixel_8_Pro_Camera`
  - `codecs=h264,hevc`
  - `api_level=34`
  - `uuid=a3b8...`
- Клієнт на Desktop (Bonjour на Windows, Avahi на Linux або вбудований socket discovery) слухає порт `5353` мультикаст-групи `224.0.0.251` і миттєво знаходить доступний пристрій у локальній мережі.

### 3.2. USB: Режими Роботи та Пріоритезація
1. **ADB Port Forwarding (Оптимально, без потреби у ручних налаштуваннях)**:
   - Клієнтський демон виконує команду:
     ```bash
     adb forward tcp:8554 tcp:8554
     adb forward tcp:8555 tcp:8555
     ```
   - Забезпечує наднизьку затримку (< 2-4 мс на передачу), незалежність від радіозавад Wi-Fi.
   - Оскільки ADB прокидає лише TCP, медіа-канал через ADB використовує потокове TCP-фреймування (packet length prefix).
2. **USB Tethering (RNDIS / CDC-NCM)**:
   - Смартфон виступає як віртуальна мережева карта (інтерфейс з IP `192.168.42.x` або `192.168.43.x`).
   - Надає повноцінний IP стек із нативною підтримкою апаратного **UDP без оверхеду ADB**.
3. **Логіка вибору каналу (Fallback Matrix)**:
   - Якщо знайдено підключений USB пристрій через ADB або RNDIS $\to$ активується USB-канал (найвища стабільність).
   - Якщо USB відсутній $\to$ активується пошук через Wi-Fi mDNS.

---

## 4. NTP-подібний Розрахунок RTT та Джиттеру

Для оцінки якості зв'язку клієнт кожні 500 мс відправляє повідомлення `FramePingMessage`:

```
Client (Desktop)                                   Server (Android)
       |                                                  |
       |--- FramePing [t1 = Client TX] ------------------>| (t2 = Server RX)
       |                                                  |
       |<-- FramePong [t1, t2, t3 = Server TX] -----------| 
(t4 = Client RX)
```

### Формули розрахунку:
$$\text{RTT} = (t_4 - t_1) - (t_3 - t_2)$$
$$\text{Clock Offset} = \frac{(t_2 - t_1) + (t_3 - t_4)}{2}$$

Згладжування RTT та джиттеру (за стандартом RFC 3550):
$$\text{SmoothedRTT} \leftarrow \text{SmoothedRTT} + 0.125 \times (\text{RTT} - \text{SmoothedRTT})$$
$$\text{Jitter} \leftarrow \text{Jitter} + 0.125 \times (|\text{RTT} - \text{SmoothedRTT}| - \text{Jitter})$$

---

## 5. Структура Репозиторію

- `proto/eyecam.proto` — Спільна cross-platform Protobuf v3 схема для Android (Java/Kotlin) та Desktop (C++/Rust/Python).
- `src/eyecam_protocol.py` — Бінарні структури заголовків та серіалізатор протоколу.
- `src/control_server.py` — Еталонний сервер контрольного каналу для Android / Linux.
- `src/control_client.py` — Клієнт керування для Desktop з Keep-Alive та RTT-метриками.
- `src/media_transport.py` — Високопродуктивний UDP рушій з фрагментацією NALU та детектором втрати кадрів (Zero-Retransmission).

---

## 6. Драйвери та Модулі Віртуальної Камери (Virtual Camera Subsystem)

### 6.1. Архітектура віртуальних пристроїв

Для того, щоб сторонні додатки (Zoom, Microsoft Teams, Discord, OBS Studio, Google Chrome, Skype) розпізнавали потік як фізичну підключену вебкамеру, розроблено дві нативні підсистеми драйверного рівня:

```
[ Відеотракт EyeCam / Продюсер ]
              │
      ┌───────┴────────────────────────┐
      ▼ (Linux)                        ▼ (Windows)
[ LinuxV4L2Sink ]             [ WindowsDirectShowSink ]
      │                                │ (Shared Memory IPC + Named Event)
      ▼ (/dev/videoX)                  ▼
[ v4l2loopback Kernel Driver ]  [ EyeCamFilter.dll DirectShow Source Filter ]
      │ (exclusive_caps=1)             │ (CLSID_VideoInputDeviceCategory + PIN_CATEGORY_CAPTURE)
      ▼                                ▼
[ Zoom / Teams / Discord / OBS / Chrome WebRTC ]
```

---

### 6.2. Linux: Модуль `LinuxV4L2Sink` (v4l2loopback)

Модуль реалізовано в класі `EyeCam::LinuxV4L2Sink` ([include/LinuxV4L2Sink.hpp](file:///c:/Users/Satana/Desktop/Project/EyeCam/include/LinuxV4L2Sink.hpp), [src/linux/LinuxV4L2Sink.cpp](file:///c:/Users/Satana/Desktop/Project/EyeCam/src/linux/LinuxV4L2Sink.cpp)).

#### Ключові технічні характеристики:
1. **Запит можливостей (Capability Queries)**:
   - Виклик `ioctl(fd, VIDIOC_QUERYCAP, &cap)`.
   - Автоматична перевірка прапорця `V4L2_CAP_VIDEO_OUTPUT` (з урахуванням `V4L2_CAP_DEVICE_CAPS` для сучасних ядер Linux).
2. **Формати пікселів**:
   - `PixelFormat::YUY2` $\to$ `V4L2_PIX_FMT_YUYV`
   - `PixelFormat::RGB24` $\to$ `V4L2_PIX_FMT_RGB24`
   - `PixelFormat::BGR24` $\to$ `V4L2_PIX_FMT_BGR24`
   - `PixelFormat::NV12` $\to$ `V4L2_PIX_FMT_NV12`
3. **Контроль частоти кадрів (30 / 60 FPS) та темпу (Pacing)**:
   - Конфігурація драйвера через `VIDIOC_S_PARM` (`timeperframe.numerator = 1`, `timeperframe.denominator = fps`).
   - Прецизійне вирівнювання темпу виводу (`WriteFramePaced`) на базі `std::chrono::steady_clock` для уникнення мікрофризів у WebRTC.
4. **Динамічна зміна роздільної здатності на льоту (Dynamic Resizing)**:
   - Підтримка переналаштування геометрії (`SetFormat` або автодетект у `WriteFrame(const VideoFrame& frame)`) через повторний виклик `VIDIOC_S_FMT` без перезавантаження дескриптора пристрою.
5. **Необхідні системні заголовки Linux**:
   - `<linux/videodev2.h>`, `<sys/ioctl.h>`, `<fcntl.h>`, `<unistd.h>`.

#### Налаштування v4l2loopback:
```bash
# Встановлення та завантаження модуля з прапорцем exclusive_caps=1
# (exclusive_caps=1 є критичним для Chrome/Zoom, щоб приховати прапорець виводу)
sudo chmod +x scripts/linux/setup_v4l2loopback.sh
sudo ./scripts/linux/setup_v4l2loopback.sh
```

---

### 6.3. Windows: Модулі `WindowsDirectShowSink` та `EyeCamFilter.dll`

На платформі Windows реалізовано повноцінний конвеєр віртуальної камери:
- **`EyeCam::WindowsDirectShowSink`**: високопродуктивний C++ продюсер ([include/WindowsDirectShowSink.hpp](file:///c:/Users/Satana/Desktop/Project/EyeCam/include/WindowsDirectShowSink.hpp), [src/windows/WindowsDirectShowSink.cpp](file:///c:/Users/Satana/Desktop/Project/EyeCam/src/windows/WindowsDirectShowSink.cpp)).
- **`EyeCamFilter.dll`**: In-process COM-сервер DirectShow Capture Source Filter ([src/windows/filter/EyeCamFilter.hpp](file:///c:/Users/Satana/Desktop/Project/EyeCam/src/windows/filter/EyeCamFilter.hpp), [src/windows/filter/EyeCamFilter.cpp](file:///c:/Users/Satana/Desktop/Project/EyeCam/src/windows/filter/EyeCamFilter.cpp)).

#### Чому сторонні додатки (Zoom, Teams, Discord, OBS) визначають його як фізичну вебкамеру:
1. **Реєстрація в категорії пристроїв введення відео**:
   - Фільтр реєструється у системній категорії DirectShow `CLSID_VideoInputDeviceCategory` (`{860BB310-5D01-11d0-BD3B-00A0C911CE86}`) через `IFilterMapper2::RegisterFilter`.
2. **Ідентифікація піна захоплення (`IKsPropertySet`)**:
   - Вихідний пін реалізує інтерфейс `IKsPropertySet` для набору властивостей `AMPROPSETID_Pin` (`dwPropID = AMPMS_PIN_CATEGORY`) та повертає `PIN_CATEGORY_CAPTURE`.
   - **Це критично важливо**: Zoom, Microsoft Teams та Chrome відкидають будь-які джерела без категорії `PIN_CATEGORY_CAPTURE`!
3. **Узгодження параметрів через `IAMStreamConfig`**:
   - Підтримка вибору роздільної здатності (1080p, 720p, 480p) та FPS (30/60) за запитом клієнтського додатку.
4. **Високошвидкісний IPC-міст (Shared Memory)**:
   - Відображення секції пам'яті (`CreateFileMappingW` / `MapViewOfFile`) з підтримкою Global/Local префіксів.
   - Миттєва сигналізація появи кадру через системну подію `SetEvent(m_hFrameEvent)` із затримкою передачі < 0.2 мс.
   - Повна сумісність як з простим буфером, так і з безблокувальним потрійним буфером (Triple Buffering).
5. **Необхідні системні заголовки Windows SDK**:
   - `<dshow.h>`, `<ks.h>`, `<ksmedia.h>`, `<strmif.h>`, `<initguid.h>`.
   - Бібліотеки: `strmiids.lib`, `ole32.lib`, `oleaut32.lib`, `user32.lib`, `shlwapi.lib`.

#### Інсталяція та реєстрація фільтра:
```cmd
:: Запустіть від імені Адміністратора:
scripts\windows\register_filter.bat

:: Для дереєстрації:
scripts\windows\unregister_filter.bat
```

#### Modern Media Foundation Virtual Camera (Windows 11 / Windows 10 21H2+):
Для керування та перевірки нативних Media Foundation камер використовуйте PowerShell скрипт:
```powershell
powershell -ExecutionPolicy Bypass -File scripts\windows\register_mf_virtualcam.ps1 -Action List
```

---

### 6.4. Майстер-скрипти збірки та документація

Для повної та покрокової інструкції зі збірки всіх підсистем перегляньте [BUILD_GUIDE.md](file:///c:/Users/Satana/Desktop/Project/EyeCam/BUILD_GUIDE.md).

#### Збірка на Windows (One-Click Master Script):
```cmd
build_windows.bat Release
```
Скрипт автоматично визначить середовище MSVC 2022, сконфігурує CMake, збере DirectShow Filter DLL (`EyeCamFilter.dll`), тестові утиліти та студійний додаток. Для реєстрації фільтра у системі запустіть:
```cmd
regsvr32.exe build_windows\bin\Release\EyeCamFilter.dll
```

#### Збірка на Linux (One-Click Master Script):
```bash
chmod +x build_linux.sh scripts/linux/setup_v4l2loopback.sh
./build_linux.sh Release

# Ініціалізація ноди /dev/video10 для v4l2loopback
sudo ./scripts/linux/setup_v4l2loopback.sh

# Запуск тесту стрімінгу
./build_linux/bin/linux_test /dev/video10
```

#### Збірка Android App:
```bash
./gradlew assembleDebug
adb install -r build/outputs/apk/debug/EyeCam-debug.apk
```
