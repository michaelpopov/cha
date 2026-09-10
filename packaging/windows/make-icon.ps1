param(
    [Parameter(Mandatory = $true)]
    [string]$Output
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ChaNativeIcon {
    [DllImport("user32.dll")]
    public static extern bool DestroyIcon(IntPtr handle);
}
'@

$directory = Split-Path -Parent $Output
New-Item -ItemType Directory -Path $directory -Force | Out-Null

$size = 256
$bitmap = [System.Drawing.Bitmap]::new($size, $size)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$graphics.Clear([System.Drawing.Color]::Transparent)

$background = [System.Drawing.Drawing2D.GraphicsPath]::new()
$margin = 10
$diameter = 112
$right = $size - $margin
$bottom = $size - $margin
$background.AddArc($margin, $margin, $diameter, $diameter, 180, 90)
$background.AddArc($right - $diameter, $margin, $diameter, $diameter, 270, 90)
$background.AddArc($right - $diameter, $bottom - $diameter, $diameter, $diameter, 0, 90)
$background.AddArc($margin, $bottom - $diameter, $diameter, $diameter, 90, 90)
$background.CloseFigure()
$brush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 17, 24, 39))
$graphics.FillPath($brush, $background)

$mark = [System.Drawing.Drawing2D.GraphicsPath]::new()
$mark.StartFigure()
$mark.AddBezier(176, 176, 116, 224, 56, 176, 56, 128)
$mark.AddBezier(56, 128, 56, 80, 116, 32, 176, 80)
$pen = [System.Drawing.Pen]::new([System.Drawing.Color]::White, 28)
$pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$graphics.DrawPath($pen, $mark)

$handle = $bitmap.GetHicon()
try {
    $icon = [System.Drawing.Icon]::FromHandle($handle)
    $stream = [System.IO.File]::Open($Output, [System.IO.FileMode]::Create)
    try {
        $icon.Save($stream)
    } finally {
        $stream.Dispose()
        $icon.Dispose()
    }
} finally {
    [ChaNativeIcon]::DestroyIcon($handle) | Out-Null
    $pen.Dispose()
    $mark.Dispose()
    $brush.Dispose()
    $background.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
}
