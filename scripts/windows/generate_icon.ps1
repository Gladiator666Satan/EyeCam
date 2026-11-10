# ==============================================================================
# EyeCam Studio — Multi-Resolution Windows Icon Generator (.ico)
# Renders the EyeCam Cyber-Eye with Camera Lens Glare inside a Laptop Bezel Frame
# ==============================================================================
param(
    [string]$OutputPath = "$PSScriptRoot\..\..\src\windows\gui\eyecam_pc.ico"
)

Add-Type -AssemblyName System.Drawing

function Draw-EyeCamLaptopIcon([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $s = [float]$size / 256.0

    # 1. Outer Dark Studio Badge (Rounded Squircle)
    $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF(0, 0)),
        (New-Object System.Drawing.PointF($size, $size)),
        [System.Drawing.Color]::FromArgb(255, 11, 14, 20),
        [System.Drawing.Color]::FromArgb(255, 19, 26, 38)
    )
    $pathBg = New-Object System.Drawing.Drawing2D.GraphicsPath
    $r = 52.0 * $s
    $pad = 8.0 * $s
    $w = $size - 2 * $pad
    $pathBg.AddArc($pad, $pad, $r, $r, 180, 90)
    $pathBg.AddArc($pad + $w - $r, $pad, $r, $r, 270, 90)
    $pathBg.AddArc($pad + $w - $r, $pad + $w - $r, $r, $r, 0, 90)
    $pathBg.AddArc($pad, $pad + $w - $r, $r, $r, 90, 90)
    $pathBg.CloseFigure()
    $g.FillPath($bgBrush, $pathBg)

    $borderPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(190, 0, 229, 255), (3.5 * $s))
    $g.DrawPath($borderPen, $pathBg)

    # 2. Laptop Screen Bezel Frame (Рамка як від ноутбука)
    $scrX = 26.0 * $s
    $scrY = 36.0 * $s
    $scrW = 204.0 * $s
    $scrH = 144.0 * $s
    $bezelBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 24, 33, 48))
    $bezelPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 0, 229, 255), (4.5 * $s))
    $g.FillRectangle($bezelBrush, $scrX, $scrY, $scrW, $scrH)
    $g.DrawRectangle($bezelPen, $scrX, $scrY, $scrW, $scrH)

    # Inner Laptop Display Area
    $inX = 35.0 * $s
    $inY = 47.0 * $s
    $inW = 186.0 * $s
    $inH = 124.0 * $s
    $dispBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 8, 11, 18))
    $g.FillRectangle($dispBrush, $inX, $inY, $inW, $inH)

    # Top Webcam indicator dot on Laptop Bezel
    $camDotBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 0, 230, 118))
    $g.FillEllipse($camDotBrush, (128.0 - 3.2) * $s, 39.0 * $s, 6.4 * $s, 6.4 * $s)

    # Laptop Keyboard Base / Deck at Bottom
    $basePath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $basePath.AddPolygon(@(
        (New-Object System.Drawing.PointF((12.0 * $s), (183.0 * $s))),
        (New-Object System.Drawing.PointF((244.0 * $s), (183.0 * $s))),
        (New-Object System.Drawing.PointF((228.0 * $s), (206.0 * $s))),
        (New-Object System.Drawing.PointF((28.0 * $s), (206.0 * $s)))
    ))
    $baseBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF(0, (180.0 * $s))),
        (New-Object System.Drawing.PointF(0, (208.0 * $s))),
        [System.Drawing.Color]::FromArgb(255, 45, 58, 78),
        [System.Drawing.Color]::FromArgb(255, 22, 30, 44)
    )
    $basePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 0, 229, 255), (3.0 * $s))
    $g.FillPath($baseBrush, $basePath)
    $g.DrawPath($basePen, $basePath)

    # Laptop Touchpad Notch Glow
    $notchPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 0, 230, 118), (4.0 * $s))
    $g.DrawLine($notchPen, (104.0 * $s), (188.0 * $s), (152.0 * $s), (188.0 * $s))

    # 3. Eye Contour inside Laptop Screen (Око)
    $eyePath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $eyePath.AddBezier(
        (New-Object System.Drawing.PointF((50.0 * $s), (109.0 * $s))),
        (New-Object System.Drawing.PointF((88.0 * $s), (53.0 * $s))),
        (New-Object System.Drawing.PointF((168.0 * $s), (53.0 * $s))),
        (New-Object System.Drawing.PointF((206.0 * $s), (109.0 * $s)))
    )
    $eyePath.AddBezier(
        (New-Object System.Drawing.PointF((206.0 * $s), (109.0 * $s))),
        (New-Object System.Drawing.PointF((168.0 * $s), (165.0 * $s))),
        (New-Object System.Drawing.PointF((88.0 * $s), (165.0 * $s))),
        (New-Object System.Drawing.PointF((50.0 * $s), (109.0 * $s)))
    )
    $eyeFill = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 14, 24, 36))
    $eyePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 0, 230, 118), (6.0 * $s))
    $g.FillPath($eyeFill, $eyePath)
    $g.DrawPath($eyePen, $eyePath)

    # 4. Camera Lens Iris & Aperture Rings
    $cx = 128.0 * $s
    $cy = 109.0 * $s
    $irisR = 39.0 * $s
    $irisBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF(($cx - $irisR), ($cy - $irisR))),
        (New-Object System.Drawing.PointF(($cx + $irisR), ($cy + $irisR))),
        [System.Drawing.Color]::FromArgb(255, 0, 168, 218),
        [System.Drawing.Color]::FromArgb(255, 9, 32, 52)
    )
    $irisPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 0, 229, 255), (3.8 * $s))
    $g.FillEllipse($irisBrush, $cx - $irisR, $cy - $irisR, $irisR * 2, $irisR * 2)
    $g.DrawEllipse($irisPen, $cx - $irisR, $cy - $irisR, $irisR * 2, $irisR * 2)

    # Inner Emerald Ring
    $ringR = 28.0 * $s
    $ringPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(225, 0, 230, 118), (2.4 * $s))
    $g.DrawEllipse($ringPen, $cx - $ringR, $cy - $ringR, $ringR * 2, $ringR * 2)

    # Deep Optical Pupil
    $pupilR = 18.5 * $s
    $pupilBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 4, 7, 12))
    $g.FillEllipse($pupilBrush, $cx - $pupilR, $cy - $pupilR, $pupilR * 2, $pupilR * 2)

    # 5. Camera Lens Glare / Reflection (Блік як від об'єктиву)
    $glareBrush1 = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(250, 255, 255, 255))
    $g.FillEllipse($glareBrush1, (104.0 * $s), (83.0 * $s), (18.0 * $s), (13.0 * $s))

    $glareBrush2 = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(210, 140, 245, 255))
    $g.FillEllipse($glareBrush2, (97.0 * $s), (100.0 * $s), (7.5 * $s), (7.5 * $s))

    # Secondary bottom-right cyan/emerald optical reflection
    $glareBrush3 = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(180, 0, 230, 118))
    $g.FillEllipse($glareBrush3, (137.0 * $s), (119.0 * $s), (11.0 * $s), (7.5 * $s))

    $g.Dispose()
    return $bmp
}

$sizes = @(256, 64, 48, 32, 16)
$pngStreams = @()
foreach ($sz in $sizes) {
    $bmp = Draw-EyeCamLaptopIcon $sz
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngStreams += ,($ms.ToArray())
    $ms.Dispose()
    $bmp.Dispose()
}

$parentDir = Split-Path -Parent $OutputPath
if (-not (Test-Path $parentDir)) {
    New-Item -ItemType Directory -Path $parentDir -Force | Out-Null
}

$fs = [System.IO.File]::Create($OutputPath)
$bw = New-Object System.IO.BinaryWriter($fs)

# ICONDIR header (6 bytes)
$bw.Write([uint16]0)
$bw.Write([uint16]1)
$bw.Write([uint16]$sizes.Count)

$offset = 6 + (16 * $sizes.Count)
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $sz = $sizes[$i]
    $bytes = $pngStreams[$i]
    $wByte = if ($sz -ge 256) { [byte]0 } else { [byte]$sz }
    $bw.Write($wByte)
    $bw.Write($wByte)
    $bw.Write([byte]0)
    $bw.Write([byte]0)
    $bw.Write([uint16]1)
    $bw.Write([uint16]32)
    $bw.Write([uint32]$bytes.Length)
    $bw.Write([uint32]$offset)
    $offset += $bytes.Length
}

for ($i = 0; $i -lt $sizes.Count; $i++) {
    $bw.Write($pngStreams[$i])
}

$bw.Close()
$fs.Close()
Write-Output "[EyeCam] Generated icon: $OutputPath ($((Get-Item $OutputPath).Length) bytes)"
