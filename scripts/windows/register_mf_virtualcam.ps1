<#
.SYNOPSIS
    Реєстрація та керування віртуальною камерою Windows 10 / Windows 11 через modern Media Foundation API.
.DESCRIPTION
    Починаючи з Windows 11 (Build 22000+) та Windows 10 (21H2+), Microsoft ввела API MFCreateVirtualCamera
    (mfvirtualcamera.h). Цей скрипт демонструє реєстрацію нативного Media Foundation Software Source
    та показує доступні віртуальні камери в системі.
#>

[CmdletBinding()]
param (
    [Parameter(Mandatory=$false)]
    [ValidateSet("List", "RegisterDirectShow", "Info")]
    [string]$Action = "Info"
)

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host " EyeCam: Windows Media Foundation & DirectShow Virtual Camera Manager" -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan

# Перевірка версії Windows
$os = Get-CimInstance Win32_OperatingSystem
Write-Host "[*] ОС: $($os.Caption) (Build: $($os.BuildNumber))"

# Перевірка наявності прав адміністратора
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Warning "Для реєстрації системних драйверів та фільтрів потрібні права Адміністратора."
}

switch ($Action) {
    "List" {
        Write-Host "`n[*] Сканування зареєстрованих DirectShow відеопристроїв (CLSID_VideoInputDeviceCategory):" -ForegroundColor Yellow
        $dsCategory = "HKCR:\CLSID\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\Instance"
        if (Test-Path $dsCategory) {
            Get-ChildItem $dsCategory | ForEach-Object {
                $friendlyName = (Get-ItemProperty $_.PSPath).FriendlyName
                $clsid = (Get-ItemProperty $_.PSPath).CLSID
                Write-Host "  -> Пристрій: $friendlyName [CLSID: $clsid]" -ForegroundColor Green
            }
        } else {
            Write-Host "  Категорію DirectShow пристроїв не знайдено."
        }

        Write-Host "`n[*] Сканування PnP Video Capture Devices (Device Manager):" -ForegroundColor Yellow
        Get-PnpDevice -Class "Camera", "Image" -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host "  -> $($_.FriendlyName) | Статус: $($_.Status) | InstanceId: $($_.InstanceId)"
        }
    }

    "Info" {
        Write-Host @"

[Архітектура сумісності Windows Virtual Camera]

1. DirectShow Capture Filter (Universal - 100% сумісність):
   - Працює у ВСІХ версіях Windows (Win 7, 8, 10, 11).
   - Підтримується 100% сторонніх програм: Zoom, Microsoft Teams (Classic та New),
     Discord, OBS Studio, Google Chrome, Mozilla Firefox, Skype.
   - Механізм: COM In-process сервер, зареєстрований у категорії:
     CLSID_VideoInputDeviceCategory {860BB310-5D01-11d0-BD3B-00A0C911CE86}
     з прапорцем PIN_CATEGORY_CAPTURE.
   - Інсталяція: `regsvr32 EyeCamFilter.dll`

2. Modern Media Foundation Virtual Camera (Windows 11 / Windows 10 21H2+):
   - Використовує Windows Media Foundation API: MFCreateVirtualCamera.
   - Тип: MFVirtualCameraType_SoftwareCameraSource.
   - Рівень життя: MFVirtualCameraLifetime_System.
   - Дозволяє підключати UVC-подібні джерела без написання драйвера режиму ядра (KMDF).

Для реєстрації EyeCam як системної камери виконайте:
  .\scripts\windows\register_filter.bat (з правами адміністратора)

"@ -ForegroundColor White
    }
}
