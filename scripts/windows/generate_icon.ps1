$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
$guiDir = Join-Path $projectRoot "src\windows\gui"
if (-not (Test-Path $guiDir)) {
    New-Item -ItemType Directory -Path $guiDir -Force | Out-Null
}

$icoPath = Join-Path $guiDir "eyecam_pc.ico"
Write-Host "Creating Windows Studio Laptop+Lens Icon: $icoPath"

Add-Type -AssemblyName System.Drawing

function Generate-EyeCamBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $scale = $size / 256.0

    # 1. Корпус ноутбука (Тонка кришка дисплея)
    $dispL = 36.0 * $scale
    $dispT = 44.0 * $scale
    $dispW = 184.0 * $scale
    $dispH = 120.0 * $scale
    $dispRadius = 14.0 * $scale

    $dispBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF($dispL, $dispT)),
        (New-Object System.Drawing.PointF($dispL, $dispT + $dispH)),
        [System.Drawing.Color]::FromArgb(255, 30, 36, 48),
        [System.Drawing.Color]::FromArgb(255, 14, 18, 24)
    )

    $dispPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 0, 229, 255), [Math]::Max(1.5 * $scale, 1.0))

    $pathDisp = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $dispRadius * 2
    $pathDisp.AddArc($dispL, $dispT, $d, $d, 180, 90)
    $pathDisp.AddArc($dispL + $dispW - $d, $dispT, $d, $d, 270, 90)
    $pathDisp.AddArc($dispL + $dispW - $d, $dispT + $dispH - $d, $d, $d, 0, 90)
    $pathDisp.AddArc($dispL, $dispT + $dispH - $d, $d, $d, 90, 90)
    $pathDisp.CloseFigure()

    $g.FillPath($dispBrush, $pathDisp)
    $g.DrawPath($dispPen, $pathDisp)

    # 2. Матриця дисплея (Темний глянцевий OLED екран)
    $scrL = 46.0 * $scale
    $scrT = 54.0 * $scale
    $scrW = 164.0 * $scale
    $scrH = 100.0 * $scale
    $scrBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 6, 8, 12))
    $g.FillRectangle($scrBrush, $scrL, $scrT, $scrW, $scrH)

    # 3. База / клавіатурний блок ноутбука
    $baseL = 20.0 * $scale
    $baseT = 164.0 * $scale
    $baseW = 216.0 * $scale
    $baseH = 20.0 * $scale
    $baseRadius = 8.0 * $scale

    $baseBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF($baseL, $baseT)),
        (New-Object System.Drawing.PointF($baseL, $baseT + $baseH)),
        [System.Drawing.Color]::FromArgb(255, 45, 52, 68),
        [System.Drawing.Color]::FromArgb(255, 18, 22, 30)
    )
    $basePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 70, 80, 100), [Math]::Max(1.0 * $scale, 1.0))

    $pathBase = New-Object System.Drawing.Drawing2D.GraphicsPath
    $bd = $baseRadius * 2
    $pathBase.AddArc($baseL, $baseT, $bd, $bd, 180, 90)
    $pathBase.AddArc($baseL + $baseW - $bd, $baseT, $bd, $bd, 270, 90)
    $pathBase.AddArc($baseL + $baseW - $bd, $baseT + $baseH - $bd, $bd, $bd, 0, 90)
    $pathBase.AddArc($baseL, $baseT + $baseH - $bd, $bd, $bd, 90, 90)
    $pathBase.CloseFigure()
    $g.FillPath($baseBrush, $pathBase)
    $g.DrawPath($basePen, $pathBase)

    # Виїмка для відкривання кришки
    $notchW = 36.0 * $scale
    $notchH = 4.0 * $scale
    $notchL = 128.0 * $scale - ($notchW / 2.0)
    $notchBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 0, 229, 255))
    $g.FillRectangle($notchBrush, $notchL, $baseT, $notchW, $notchH)

    # 4. Центр: Око-об'єктив камери всередині екрану
    $eyeCx = 128.0 * $scale
    $eyeCy = 104.0 * $scale

    # Зовнішній обвід об'єктива (Neon Cyan & Emerald ring)
    $ringR = 34.0 * $scale
    $ringPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 0, 229, 255), 3.0 * $scale)
    $g.DrawEllipse($ringPen, $eyeCx - $ringR, $eyeCy - $ringR, $ringR * 2, $ringR * 2)

    # Градієнтне скло лінзи
    $lensR = 31.0 * $scale
    $lensBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF($eyeCx - $lensR, $eyeCy - $lensR)),
        (New-Object System.Drawing.PointF($eyeCx + $lensR, $eyeCy + $lensR)),
        [System.Drawing.Color]::FromArgb(255, 0, 168, 143),
        [System.Drawing.Color]::FromArgb(255, 2, 28, 48)
    )
    $g.FillEllipse($lensBrush, $eyeCx - $lensR, $eyeCy - $lensR, $lensR * 2, $lensR * 2)

    # Зіниця камери
    $pupilR = 15.0 * $scale
    $pupilBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 4, 8, 14))
    $g.FillEllipse($pupilBrush, $eyeCx - $pupilR, $eyeCy - $pupilR, $pupilR * 2, $pupilR * 2)

    # Блік лінзи (Studio Optical Lens Glare)
    $glareR = 6.0 * $scale
    $glareBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(230, 255, 255, 255))
    $g.FillEllipse($glareBrush, $eyeCx - 8.0 * $scale, $eyeCy - 9.0 * $scale, $glareR * 2, $glareR * 2)

    $microGlareR = 2.5 * $scale
    $g.FillEllipse($glareBrush, $eyeCx + 5.0 * $scale, $eyeCy + 4.0 * $scale, $microGlareR * 2, $microGlareR * 2)

    # Зелений індикатор активності камери (Top Bezel LED)
    $ledR = 2.5 * $scale
    $ledBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 0, 230, 118))
    $g.FillEllipse($ledBrush, 128.0 * $scale - $ledR, 48.0 * $scale - $ledR, $ledR * 2, $ledR * 2)

    $g.Dispose()
    return $bmp
}

$sizes = @(16, 32, 48, 64, 128, 256)
$bitmaps = @()
foreach ($sz in $sizes) {
    $bitmaps += Generate-EyeCamBitmap -size $sz
}

# Запис мультирозмірного .ICO файлу
$fs = [System.IO.File]::Create($icoPath)
$bw = New-Object System.IO.BinaryWriter($fs)

# ICONDIR
$bw.Write([uint16]0) # Reserved
$bw.Write([uint16]1) # Type 1 = ICO
$bw.Write([uint16]$sizes.Count) # Image count

$offset = 6 + ($sizes.Count * 16)
$pngStreams = @()

foreach ($bmp in $bitmaps) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $ms.ToArray()
    $pngStreams += ,$bytes
}

for ($i = 0; $i -lt $sizes.Count; $i++) {
    $sz = $sizes[$i]
    $dataLen = $pngStreams[$i].Length

    $bW = if ($sz -ge 256) { 0 } else { [byte]$sz }
    $bH = if ($sz -ge 256) { 0 } else { [byte]$sz }

    # ICONDIRENTRY
    $bw.Write([byte]$bW)
    $bw.Write([byte]$bH)
    $bw.Write([byte]0)   # Color count
    $bw.Write([byte]0)   # Reserved
    $bw.Write([uint16]1) # Color planes
    $bw.Write([uint16]32)# Bit count
    $bw.Write([uint32]$dataLen)
    $bw.Write([uint32]$offset)

    $offset += $dataLen
}

foreach ($bytes in $pngStreams) {
    $bw.Write($bytes)
}

$bw.Flush()
$bw.Close()
$fs.Close()

foreach ($bmp in $bitmaps) {
    $bmp.Dispose()
}

Write-Host "EyeCam Studio Multi-res ICO generated successfully: $icoPath"
