cmake_minimum_required(VERSION 3.20)
project(EyeCam VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Директорії для збереження зібраних бінарних файлів та бібліотек
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Опції компілятора
if (MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DNOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# ==============================================================================
# Секція 1: Linux V4L2 Loopback Virtual Camera
# ==============================================================================
if (UNIX AND NOT APPLE)
    message(STATUS "[EyeCam] Конфігурація для Linux: V4L2 Virtual Camera Sink")

    add_library(EyeCamLinuxSink STATIC
        src/linux/LinuxV4L2Sink.cpp
    )
    target_include_directories(EyeCamLinuxSink PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(EyeCamLinuxSink PUBLIC pthread)

    add_executable(linux_test
        src/examples/linux_test.cpp
    )
    target_link_libraries(linux_test PRIVATE EyeCamLinuxSink pthread)
endif()

# ==============================================================================
# Секція 2: Windows DirectShow Capture Filter & Sink
# ==============================================================================
if (WIN32)
    message(STATUS "[EyeCam] Конфігурація для Windows: DirectShow Filter & Shared Memory Sink")

    # Бібліотека продюсера для інтеграції у власні C++ додатки
    add_library(EyeCamWindowsSink STATIC
        src/windows/WindowsDirectShowSink.cpp
    )
    target_include_directories(EyeCamWindowsSink PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    # DirectShow Capture Source Filter (DLL для реєстрації через regsvr32)
    add_library(EyeCamFilter SHARED
        src/windows/filter/EyeCamFilter.cpp
        src/windows/filter/DllMain.cpp
        src/windows/filter/EyeCamFilter.def
    )
    target_include_directories(EyeCamFilter PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/windows/filter
    )
    target_link_libraries(EyeCamFilter PRIVATE
        strmiids.lib
        ole32.lib
        oleaut32.lib
        user32.lib
        shlwapi.lib
    )

    # Десктопний GUI додаток у стилі Windows 11 Dark Mode (Direct2D / DirectWrite)
    add_executable(EyeCamStudio WIN32
        src/windows/gui/EyeCamStudioWin32.cpp
    )
    target_include_directories(EyeCamStudio PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(EyeCamStudio PRIVATE
        EyeCamWindowsSink
        d2d1.lib
        dwrite.lib
        dwmapi.lib
        ws2_32.lib
        shell32.lib
        ole32.lib
        advapi32.lib
        user32.lib
        gdi32.lib
    )

    # Тестовий додаток стрімінгу для Windows
    add_executable(windows_test
        src/examples/windows_test.cpp
    )
    target_link_libraries(windows_test PRIVATE EyeCamWindowsSink)
endif()

# ==============================================================================
# Секція 3: Десктопний додаток EyeCam Studio (Qt 6 / QML / RHI)
# ==============================================================================
option(BUILD_EYECAM_APP "Збирати десктопний додаток EyeCam на базі Qt 6" ON)

find_package(Qt6 6.5 QUIET COMPONENTS
    Core
    Gui
    Quick
    QuickControls2
    Network
    Concurrent
)

if (BUILD_EYECAM_APP AND Qt6_FOUND)
    message(STATUS "[EyeCam] Знайдено Qt6. Додавання таргета EyeCam Studio...")

    # Опціональний пошук GStreamer та FFmpeg
    find_package(PkgConfig QUIET)
    if (PKG_CONFIG_FOUND)
        pkg_check_modules(GST gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0)
        pkg_check_modules(FFMPEG libavcodec libavutil libavformat libswscale)
    endif()

    qt_standard_project_setup(REQUIRES 6.5)

    qt_add_executable(EyeCam
        src/main.cpp
        src/core/AppState.hpp
        src/core/AppState.cpp
        src/core/TelemetryService.hpp
        src/core/TelemetryService.cpp
        src/core/DeviceDiscoveryService.hpp
        src/core/DeviceDiscoveryService.cpp
        src/pipeline/VideoPipeline.hpp
        src/pipeline/VideoPipeline.cpp
        src/pipeline/VirtualCameraSink.hpp
        src/pipeline/VirtualCameraSink.cpp
        src/pipeline/VideoTextureProvider.hpp
        src/pipeline/VideoTextureProvider.cpp
        src/network/MediaReceiver.hpp
        src/network/MediaReceiver.cpp
        src/network/ControlClient.hpp
        src/network/ControlClient.cpp
    )

    qt_add_qml_module(EyeCam
        URI EyeCam.UI
        VERSION 1.0
        RESOURCE_PREFIX "/"
        QML_FILES
            src/ui/qml/Main.qml
            src/ui/qml/Theme.qml
            src/ui/qml/components/GlassCard.qml
            src/ui/qml/components/GlassSlider.qml
            src/ui/qml/components/TelemetryBadge.qml
            src/ui/qml/views/LeftSidebar.qml
            src/ui/qml/views/CenterViewport.qml
            src/ui/qml/views/RightInspector.qml
    )

    target_include_directories(EyeCam PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(EyeCam PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Network
        Qt6::Concurrent
    )

    if (WIN32)
        target_link_libraries(EyeCam PRIVATE d3d11 dxgi EyeCamWindowsSink ws2_32)
        target_compile_definitions(EyeCam PRIVATE EYECAM_PLATFORM_WINDOWS)
    elseif (UNIX AND NOT APPLE)
        target_link_libraries(EyeCam PRIVATE EyeCamLinuxSink pthread)
        target_compile_definitions(EyeCam PRIVATE EYECAM_PLATFORM_LINUX)
    endif()

    if (GST_FOUND)
        target_include_directories(EyeCam PRIVATE ${GST_INCLUDE_DIRS})
        target_link_libraries(EyeCam PRIVATE ${GST_LIBRARIES})
        target_compile_definitions(EyeCam PRIVATE EYECAM_HAS_GSTREAMER)
    endif()

    set_target_properties(EyeCam PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER my.eyecam.app
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
        WIN32_EXECUTABLE TRUE
    )
else()
    message(STATUS "[EyeCam] Qt 6 не знайдено або вимкнено BUILD_EYECAM_APP. Додаток EyeCam Studio пропущено.")
    message(STATUS "[EyeCam] Модулі віртуальної камери та тест-утиліти згенеровано успішно.")
endif()
