param(
    [string]$Out = "shot.png",
    [int]$X = 0,
    [int]$Y = 0,
    [int]$W = 0,
    [int]$H = 0,
    [int]$Scale = 1
)

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class DpiHelper { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[DpiHelper]::SetProcessDPIAware() | Out-Null

$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bitmap = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)

if ($W -gt 0 -and $H -gt 0) {
    $X = [Math]::Max(0, [Math]::Min($X, $bounds.Width - 1))
    $Y = [Math]::Max(0, [Math]::Min($Y, $bounds.Height - 1))
    $W = [Math]::Min($W, $bounds.Width - $X)
    $H = [Math]::Min($H, $bounds.Height - $Y)
    $rect = New-Object System.Drawing.Rectangle($X, $Y, $W, $H)
    $crop = $bitmap.Clone($rect, $bitmap.PixelFormat)
    if ($Scale -gt 1) {
        $big = New-Object System.Drawing.Bitmap(($W * $Scale), ($H * $Scale))
        $g2 = [System.Drawing.Graphics]::FromImage($big)
        $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
        $g2.DrawImage($crop, 0, 0, $W * $Scale, $H * $Scale)
        $big.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
        $g2.Dispose()
        $big.Dispose()
    } else {
        $crop.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    $crop.Dispose()
} else {
    $bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
}

$graphics.Dispose()
$bitmap.Dispose()
"$($bounds.Width)x$($bounds.Height) -> $Out"
