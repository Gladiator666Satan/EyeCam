cmake_minimum_required(VERSION 3.22)
project(EyeCam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Глобальні прапорці компіляції для максимальної оптимізації та безпеки
if (MSVC)
    add_compile_options(/W4 /WX- /permissive- /utf-8 /EHsc /O2 /Oi /Ot /GL)
    add_link_options(/LTCG)
else()
    add_compile_options(-Wall -Wextra -pedantic -Werror -O3 -march=native -flto)
endif()

# Глобальні директорії заголовків
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ==============================================================================
# Секція 1: Linux V4L2 Loopback Sink & Shared Memory Producer
# ==============================================================================
if (UNIX AND NOT APPLE AND NOT ANDROID)
    message(STATUS "[EyeCam] Конфігурація для Linux: V4L2 Loopback & POSIX SHM")
    
    # Бібліотека продюсера для інтеграції у власні додатки
    add_library(EyeCamLinuxSink STATIC
        src/linux/LinuxV4L2Sink.cpp
    )
    target_include_directories(EyeCamLinuxSink PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(EyeCamLinuxSink PUBLIC
        pthread
        rt
    )

    # Тестовий додаток стрімінгу для перевірки V4L2 у OBS / VLC / Chrome
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

    # Десктопний GUI додаток у стилі Windows 11 Dark Mode (Direct2D / DirectWrite / WIC)
    add_executable(EyeCamStudio WIN32
        src/windows/gui/EyeCamStudioWin32.cpp
        src/windows/gui/EyeCamStudio.rc
    )
    target_include_directories(EyeCamStudio PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/windows/gui
    )
    target_link_libraries(EyeCamStudio PRIVATE
        EyeCamWindowsSink
        d2d1.lib
        dwrite.lib
        dwmapi.lib
        windowscodecs.lib
        mfplat.lib
        mf.lib
        mfuuid.lib
        wmcodecdspuuid.lib
        strmiids.lib
        ws2_32.lib
        shell32.lib
        ole32.lib
        oleaut32.lib
        uuid.lib
        advapi32.lib
        user32.lib
        gdi32.lib
    )

    # Тестовий додаток стрімінгу для Windows
    add_executable(windows_test
        src/examples/windows_test.cpp
    )
    target_link_libraries(windows_test PRIVATE
        EyeCamWindowsSink
    )
endif()
