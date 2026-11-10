# NexusCam (EyeCam) - Master Build & Integration Guide

## 1. System Overview & Architecture

**NexusCam** is a professional ultra-low-latency virtual webcam streaming platform connecting mobile devices (Android) with desktop workstations (Windows & Linux) to provide studio-grade webcam feeds (< 30 ms glass-to-glass latency).

```
+-------------------------------------------------------------------------+
|                              ANDROID DEVICE                             |
|  [Camera2 Multi-Lens] -> [MediaCodec H.264/HEVC] -> [UdpMediaSender]    |
|  [ControlServer TCP :8554] <-----------------------+                    |
+----------------------------------------------------|--------------------+
                                                     |
                                   UDP Video (:8555) | TCP Control (:8554)
                                   Wi-Fi / USB AOA   | Wi-Fi / ADB Forward
                                                     v
+-------------------------------------------------------------------------+
|                              DESKTOP ENGINE                             |
|  [MediaReceiver UDP] -> [HW Decoder NVDEC/D3D11VA]                      |
|  [ControlClient TCP] -> [NTP Ping/Pong & Telemetry]                     |
|                                |                                        |
|             +------------------+------------------+                     |
|             | (Branch A: Local UI)                | (Branch B: IPC)     |
|             v                                     v                     |
|     [Qt6 QML Viewport]              [VirtualCameraSink Triple-Buffer]   |
|                                                   |                     |
|                         +-------------------------+                     |
|                         v                                               |
|           [Windows DirectShow Filter] / [Linux v4l2loopback Sink]       |
|                         |                                               |
|                         v                                               |
|           [Zoom / Teams / OBS / Discord / WebRTC]                       |
+-------------------------------------------------------------------------+
```

---

## 2. Definitive Mono-Repo Layout

```
NexusCam/
├── CMakeLists.txt                      # Root Desktop CMake (Windows & Linux)
├── build.gradle.kts                    # Root / Application Gradle configuration
├── settings.gradle.kts                 # Gradle project inclusion settings
├── build_windows.bat                   # Master Windows compilation script
├── build_linux.sh                      # Master Linux compilation script
├── BUILD_GUIDE.md                      # This comprehensive build manual
├── README.md                           # Project introduction & high-level spec
├── ARCHITECTURE.md                     # Microkernel & reactive state architecture
│
├── include/                            # Public cross-platform C++ headers
│   ├── CommonTypes.hpp                 # Resolutions, PixelFormat, frame helpers
│   ├── NetworkProtocol.hpp             # 24-byte UDP & 8-byte TCP wire contracts
│   ├── SharedMemoryProtocol.hpp        # Zero-lock IPC triple-buffering structures
│   ├── WindowsDirectShowSink.hpp       # DirectShow producer IPC client
│   └── LinuxV4L2Sink.hpp               # Linux v4l2loopback kernel sink driver
│
├── proto/                              # Protobuf definitions
│   └── eyecam.proto                    # Binary schema for control plane & telemetry
│
├── scripts/                            # Operational automation scripts
│   ├── linux/
│   │   └── setup_v4l2loopback.sh       # DKMS module loader & device node setup
│   ├── windows/
│   │   ├── register_filter.bat         # DirectShow filter COM registration
│   │   ├── unregister_filter.bat       # Filter cleanup
│   │   └── register_mf_virtualcam.ps1  # Media Foundation virtual camera registrar
│   └── python/
│       ├── eyecam_protocol.py          # Reference binary protocol pack/unpack
│       ├── media_transport.py          # UDP sender/receiver reference
│       ├── control_server.py           # Android-side TCP daemon reference
│       └── control_client.py           # Desktop-side TCP client reference
│
├── src/                                # Source implementations
│   ├── main.cpp                        # Desktop Studio entry point (Qt6 / RHI)
│   ├── core/                           # Reactive state & hardware discovery
│   │   ├── AppState.hpp / .cpp         # Central reactive state (Q_PROPERTY)
│   │   ├── TelemetryService.hpp / .cpp # EMA-filtered telemetry (RTT, FPS, loss)
│   │   └── DeviceDiscoveryService.hpp  # mDNS (Wi-Fi) & ADB USB poller
│   ├── network/                        # Desktop C++ networking engine
│   │   ├── MediaReceiver.hpp / .cpp    # Zero-retransmission UDP receiver
│   │   └── ControlClient.hpp / .cpp    # Bidirectional TCP client & NTP sync
│   ├── pipeline/                       # Hardware video routing & rendering
│   │   ├── VideoPipeline.hpp / .cpp    # Decoder & dual-branch Tee splitter
│   │   ├── VideoTextureProvider.hpp    # Qt RHI Direct3D11/OpenGL texture provider
│   │   └── VirtualCameraSink.hpp / .cpp# IPC Shared Memory producer
│   ├── ui/                             # UI layer
│   │   └── qml/                        # Qt Quick 6 Glassmorphic interface
│   ├── windows/                        # Windows virtual webcam engine
│   │   ├── WindowsDirectShowSink.cpp   # DirectShow producer implementation
│   │   └── filter/                     # In-process COM DirectShow source filter
│   │       ├── EyeCamFilter.hpp / .cpp # IBaseFilter & IPin implementation
│   │       ├── DllMain.cpp             # DllRegisterServer / DllGetClassObject
│   │       ├── EyeCamFilter.def        # Linker export definition
│   │       └── EyeCamGuids.hpp         # CLSIDs & media subtype identifiers
│   ├── linux/                          # Linux virtual webcam engine
│   │   └── LinuxV4L2Sink.cpp           # IOCTL /dev/videoX sink implementation
│   ├── examples/                       # Standalone testing executables
│   │   ├── windows_test.cpp            # Windows IPC producer benchmark
│   │   └── linux_test.cpp              # Linux V4L2 loopback test runner
│   └── main/                           # Android core application
│       ├── AndroidManifest.xml         # Permissions (Camera, Service, Network)
│       └── java/com/eyecam/
│           ├── camera/                 # Camera2 low-latency multi-lens controller
│           ├── encoder/                # MediaCodec H.264/HEVC hardware encoder
│           ├── network/                # Kotlin UDP sender & ControlServer daemon
│           ├── pipeline/               # Surface & stream coordinator
│           ├── service/                # Foreground Service & PowerLock manager
│           ├── model/                  # Kotlin data classes & telemetry models
│           ├── ui/                     # Jetpack Compose mobile HUD & overlays
│           └── viewmodel/              # Reactive Compose view models
```

---

## 3. Wire Protocol & Interface Contracts

### 3.1. UDP Media Framing (24-byte Canonical Header)

All multi-byte fields are transmitted in **Network Byte Order (Big-Endian)**.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Magic ('EC')        |     Flags     |   Codec Type  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         Sequence Number       |           Frame ID            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Frame ID (cont.)        |         Fragment Index        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         Fragment Total        |                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               +
|                 Presentation Timestamp (PTS in us)            |
|                         (64-bit uint)                         |
+                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                               |         Payload Length        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

* **Magic**: `0x45 0x43` (ASCII `"EC"`).
* **Flags**:
  * `0x01` : Keyframe (IDR / SPS / PPS).
  * `0x02` : Start of Frame (first NALU chunk).
  * `0x04` : End of Frame (final NALU chunk).
* **Codec Type**: `1` = H.264 (AVC), `2` = H.265 (HEVC), `3` = AV1.
* **Sequence Number** (uint16): Monotonically increasing datagram counter used for packet loss detection.
* **Frame ID** (uint32): Monotonically increasing frame counter.
* **Fragment Index** (uint16): 0-indexed fragment number within the current frame.
* **Fragment Total** (uint16): Total fragments for this frame.
* **PTS** (uint64): Presentation timestamp in microseconds from monotonic clock.
* **Payload Length** (uint16): Chunk size in bytes (max 1400 bytes).

### 3.2. TCP Control Framing & JSON-RPC Dictionary

The control channel operates over TCP with `TCP_NODELAY`. Each message is preceded by an 8-byte binary header:

```
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Magic ('CP')        |          Message Type         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Payload Length (uint32)                   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

* **Magic**: `0x43 0x50` (ASCII `"CP"`).
* **Message Types**:
  * `1` : `JSON_COMMAND`
  * `2` : `PROTOBUF_COMMAND`
  * `3` : `FRAME_PING` (NTP latency ping: `{"t1_us": <timestamp>}`)
  * `4` : `FRAME_PONG` (NTP latency pong: `{"t1_us": <t1>, "t2_us": <t2>, "t3_us": <t3>}`)
  * `5` : `KEYFRAME_REQUEST` (Instant IDR request)
  * `6` : `KEEP_ALIVE`

#### Reconciled JSON-RPC Command Dictionary

| Action | Parameters | Description |
|---|---|---|
| `SetCameraModule` | `module: string` (`"0"` = Wide, `"1"` = Ultra-Wide, `"2"` = Telephoto) | Switches physical camera sensor |
| `SetISO` | `auto: bool`, `iso: int` (50–6400) | Toggles Auto-ISO or applies manual sensitivity |
| `SetShutter` | `auto: bool`, `exposure_ns: int64` | Sets exposure time (e.g. 20000000 for 1/50s) |
| `SetFocus` | `mode: string` (`"AUTO"`/`"MANUAL"`), `distance_diopters: float` | Lens focus distance (0.0 = infinity) |
| `SetWhiteBalance`| `mode: string` (`"AUTO"`/`"MANUAL_KELVIN"`), `kelvin: int` (2800–10000)| Adjusts color temperature |
| `KeyframeRequest`| `reason: string`, `last_received_frame_id: int` | Requests instant IDR frame generation |

### 3.3. Inter-Process Shared Memory (DirectShow & V4L2)

* **Windows**: Named file mapping `Global\EyeCam_VirtualCamera_Shm` with named synchronization event `Global\EyeCam_VirtualCamera_Event`.
* **Zero-Lock Triple Buffering**: Active buffer index (`0`, `1`, `2`) updated atomically in `TripleBufferShmHeader`.
* **Pixel Format Conversion**: The hardware video decoder outputs planar `NV12` (12 bpp). The DirectShow filter automatically converts `NV12` to packed `YUY2` (16 bpp) or `RGB24` in-place when connected to standard client software (Zoom, Teams, Discord).

---

## 4. Prerequisites & Environment Setup

### 4.1. Windows Workstation
1. **Operating System**: Windows 10/11 64-bit (x64).
2. **Visual Studio**: Visual Studio 2022 (Community or higher) with:
   - "Desktop development with C++"
   - MSVC v143 toolset (C++20 compliant)
   - Windows 10/11 SDK (version 10.0.22000.0 or newer)
3. **CMake**: Version 3.20 or newer (in `PATH`).
4. **Qt 6 (Optional for Desktop Studio UI)**: Qt 6.5+ (MSVC 2019/2022 64-bit) with Quick, QuickControls2, and Network modules.

### 4.2. Linux Workstation
1. **Operating System**: Ubuntu 22.04 LTS / 24.04 LTS, Debian 12, or Arch Linux.
2. **Core Toolchain**:
   ```bash
   sudo apt update
   sudo apt install -y build-essential cmake ninja-build pkg-config clang
   ```
3. **Kernel Modules & V4L2 Utilities**:
   ```bash
   sudo apt install -y v4l2loopback-dkms v4l-utils linux-headers-$(uname -r)
   ```
4. **Qt 6 & Codecs (Optional for Desktop Studio UI)**:
   ```bash
   sudo apt install -y qt6-base-dev qt6-declarative-dev libqt6network6 \
                       libva-dev libdrm-dev ffmpeg libavcodec-dev libavutil-dev
   ```

### 4.3. Android Build Environment
1. **JDK**: OpenJDK 17.
2. **Android SDK**: API Level 35 (Android 15), Minimum SDK 26 (Android 8.0).
3. **Android NDK**: NDK version `26.1.10909125` or newer.
4. **Build Tools**: Gradle 8.9+, Android Gradle Plugin 8.7.0+.

---

## 5. Step-by-Step Compilation Guide

### 5.1. Building on Windows

#### Option A: One-Click Build Script
Run the automated batch script from the repository root:
```cmd
build_windows.bat Release
```

#### Option B: Manual CMake Invocation
Open the **Developer Command Prompt for VS 2022** and execute:
```cmd
mkdir build_windows
cd build_windows

cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release -DBUILD_EYECAM_APP=ON
cmake --build . --config Release --parallel

# Register the DirectShow Virtual Camera Filter (Admin privileges required)
regsvr32.exe bin\Release\EyeCamFilter.dll
```

#### Testing Windows Output
Launch the test IPC producer to verify virtual camera output in OBS or Zoom:
```cmd
bin\Release\windows_test.exe
```

---

### 5.2. Building on Linux

#### Option A: One-Click Build Script
Make the script executable and run:
```bash
chmod +x build_linux.sh scripts/linux/setup_v4l2loopback.sh
./build_linux.sh Release
```

#### Option B: Manual CMake Invocation
```bash
mkdir -p build_linux
cd build_linux

cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DBUILD_EYECAM_APP=ON
cmake --build . --config Release --parallel $(nproc)
```

#### Configuring the V4L2 Loopback Device
Run the driver loader with root privileges:
```bash
sudo ./scripts/linux/setup_v4l2loopback.sh
```
This loads `v4l2loopback` with `exclusive_caps=1` and registers `/dev/video10` under the name `"EyeCam Virtual Camera"`.

#### Testing Linux Output
```bash
./build_linux/bin/linux_test
```

---

### 5.3. Building the Android Application

From the project root (or inside Android Studio):

```bash
# Verify Gradle wrapper
gradle wrapper --gradle-version 8.9

# Build debug APK
./gradlew assembleDebug

# Build optimized release APK
./gradlew assembleRelease
```
The resulting APK is generated at:
`build/outputs/apk/debug/EyeCam-debug.apk`

#### Installing to Connected Phone via ADB
```bash
adb install -r build/outputs/apk/debug/EyeCam-debug.apk

# Forward TCP control plane port over USB
adb forward tcp:8554 tcp:8554

# Start streaming foreground service via intent
adb shell am start -n com.eyecam/.ui.MainActivity
```

---

## 6. Verification and Integration Testing

| Component | Test Procedure | Expected Result |
|---|---|---|
| **UDP Media Plane** | Run `scripts/python/media_transport.py --listen 8555` while Android stream is active | Packets arrive with magic `0x4543`, `pts_us` incrementing, 0% drop on USB |
| **Control Plane** | Connect using `scripts/python/control_client.py` on port 8554 | Returns RTT < 2 ms, receives `ack` for `SetISO` / `SetShutter` |
| **DirectShow Filter** | Open OBS Studio -> Add "Video Capture Device" -> Select "EyeCam Virtual Camera" | Live 60 FPS video renders with no color distortion or line shearing |
| **Linux V4L2 Sink** | Open Google Meet or Chromium -> Select "EyeCam Virtual Camera" | Smooth 1080p/60fps video feed recognized without permission warnings |
| **Keyframe Recovery** | Drop UDP datagrams artificially via firewall rule | MediaReceiver triggers `sigKeyframeRequested`, Android emits IDR within 1 frame |

---

## 7. Troubleshooting & FAQ

* **Issue: OBS / Zoom shows black screen on Windows.**
  * *Fix*: Ensure `EyeCamFilter.dll` was registered with Administrator rights via `regsvr32`. Make sure either `EyeCam.exe` or `windows_test.exe` is actively running to feed frames into the shared memory.
* **Issue: Browser / Chrome doesn't recognize Linux camera.**
  * *Fix*: The `v4l2loopback` module must be loaded with `exclusive_caps=1`. Re-run `sudo ./scripts/linux/setup_v4l2loopback.sh`.
* **Issue: Android MediaCodec error `0x80001001` on startup.**
  * *Fix*: The selected resolution is unsupported by the hardware encoder. Check `discoverAvailableSensors()` in `CameraController.kt` and use supported resolutions (1920x1080 or 1280x720).
* **Issue: High latency or buffer buildup over Wi-Fi.**
  * *Fix*: Switch to 5 GHz Wi-Fi or connect via USB with `adb forward tcp:8554 tcp:8554`. NexusCam enforces a zero-retransmission policy; ensure the router does not perform 802.11 aggregation retries.
