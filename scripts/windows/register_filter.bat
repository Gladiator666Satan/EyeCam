@echo off
:: ==============================================================================
:: Скрипт реєстрації DirectShow Capture Source фільтра EyeCamFilter.dll
:: ==============================================================================
chcp 65001 >nul
echo ======================================================================
echo  Реєстрація віртуальної камери EyeCam Virtual Camera (DirectShow / COM)
echo ======================================================================

:: Перевірка прав адміністратора
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [-] Помилка: Запустіть цей командний рядок від імені Адміністратора!
    echo     Клацніть правою кнопкою миші на файл і виберіть 'Запуск від імені адміністратора'.
    pause
    exit /b 1
)

set SCRIPT_DIR=%~dp0
set DLL_PATH=%SCRIPT_DIR%..\..\build\bin\Release\EyeCamFilter.dll

if not exist "%DLL_PATH%" (
    set DLL_PATH=%SCRIPT_DIR%..\..\build\bin\EyeCamFilter.dll
)

if not exist "%DLL_PATH%" (
    set DLL_PATH=%SCRIPT_DIR%EyeCamFilter.dll
)

if not exist "%DLL_PATH%" (
    echo [-] Файл EyeCamFilter.dll не знайдено!
    echo     Будь ласка, спочатку скомпілюйте проект через CMake або скопіюйте DLL у цей каталог.
    echo     Очікуваний шлях: %DLL_PATH%
    pause
    exit /b 1
)

echo [*] Реєстрація бібліотеки: %DLL_PATH%
regsvr32.exe "%DLL_PATH%"

if %errorlevel% equ 0 (
    echo.
    echo [+] УСПІШНО! Фільтр зареєстровано в категорії CLSID_VideoInputDeviceCategory.
    echo     Тепер 'EyeCam Virtual Camera' з'явиться в меню вибору вебкамери у:
    echo       - Zoom
    echo       - Microsoft Teams
    echo       - Discord
    echo       - OBS Studio
    echo       - Google Chrome / Edge (WebRTC)
) else (
    echo.
    echo [-] Помилка реєстрації regsvr32 (код помилки: %errorlevel%).
)

pause
