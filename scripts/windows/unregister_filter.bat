@echo off
:: ==============================================================================
:: Скрипт дереєстрації DirectShow Capture Source фільтра EyeCamFilter.dll
:: ==============================================================================
chcp 65001 >nul
echo ======================================================================
echo  Дереєстрація віртуальної камери EyeCam (DirectShow / COM)
echo ======================================================================

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [-] Помилка: Запустіть цей командний рядок від імені Адміністратора!
    pause
    exit /b 1
)

set SCRIPT_DIR=%~dp0
set DLL_PATH=%SCRIPT_DIR%..\..\build\bin\Release\EyeCamFilter.dll

if not exist "%DLL_PATH%" (
    set DLL_PATH=%SCRIPT_DIR%..\..\build\bin\EyeCamFilter.dll
)

if not exist "%DLL_PATH%" (
    set DLL_PATH=%SCRIPT_DIR%..\EyeCamFilter.dll
)

if not exist "%DLL_PATH%" (
    set DLL_PATH=%SCRIPT_DIR%EyeCamFilter.dll
)

if not exist "%DLL_PATH%" (
    echo [-] Файл EyeCamFilter.dll не знайдено!
    pause
    exit /b 1
)

echo [*] Видалення реєстрації бібліотеки: %DLL_PATH%
regsvr32.exe /u "%DLL_PATH%"

if %errorlevel% equ 0 (
    echo.
    echo [+] Фільтр успішно видалено з системи та реєстру DirectShow.
) else (
    echo.
    echo [-] Помилка дереєстрації regsvr32 (код помилки: %errorlevel%).
)

pause
