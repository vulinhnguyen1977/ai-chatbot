<#
.SYNOPSIS
  Generates res/mstutor.ico.

.DESCRIPTION
  Draws the application icon at several sizes and assembles them into a
  multi-resolution .ico. Run once; the result is committed and the build just
  embeds it.

  A shipped application should not use the generic Windows icon: it looks
  unfinished, and "no icon, no version metadata, freshly compiled, unsigned" is
  the exact profile that trips antivirus machine-learning heuristics.
#>
param([string]$Out = (Join-Path $PSScriptRoot "mstutor.ico"))

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = "Stop"

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$frames = @()

foreach ($size in $sizes) {
  $bmp = New-Object System.Drawing.Bitmap($size, $size,
         [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
  $g.Clear([System.Drawing.Color]::Transparent)

  # Rounded square background in the app's accent blue.
  $r = [Math]::Max(2, [int]($size * 0.18))
  $pad = [Math]::Max(0, [int]($size * 0.03))
  $w = $size - 1 - ($pad * 2)
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $path.AddArc($pad, $pad, $r*2, $r*2, 180, 90)
  $path.AddArc($pad + $w - $r*2, $pad, $r*2, $r*2, 270, 90)
  $path.AddArc($pad + $w - $r*2, $pad + $w - $r*2, $r*2, $r*2, 0, 90)
  $path.AddArc($pad, $pad + $w - $r*2, $r*2, $r*2, 90, 90)
  $path.CloseFigure()

  $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
      (New-Object System.Drawing.Point(0,0)),
      (New-Object System.Drawing.Point($size,$size)),
      [System.Drawing.Color]::FromArgb(255, 47, 111, 237),
      [System.Drawing.Color]::FromArgb(255, 24, 62, 148))
  $g.FillPath($brush, $path)

  # A "pi" glyph reads as maths at every size and stays legible at 16px.
  $fontSize = [float]($size * 0.62)
  $font = New-Object System.Drawing.Font("Segoe UI", $fontSize,
          [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
  $fmt = New-Object System.Drawing.StringFormat
  $fmt.Alignment = [System.Drawing.StringAlignment]::Center
  $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
  $rect = New-Object System.Drawing.RectangleF(0, [float]($size * -0.04), $size, $size)
  $g.DrawString([char]0x03C0, $font, [System.Drawing.Brushes]::White, $rect, $fmt)

  $g.Dispose()

  $stream = New-Object System.IO.MemoryStream
  $bmp.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
  $frames += ,@($size, $stream.ToArray())
  $bmp.Dispose()
}

# --- assemble the .ico ------------------------------------------------------
# ICONDIR (6 bytes) + one ICONDIRENTRY (16 bytes) per frame + the PNG payloads.
# Windows Vista and later accept PNG-compressed frames at any size.
#
# Note on naming: PowerShell variables are case-insensitive, so a local called
# $out would be the same variable as the $Out parameter.
$icoStream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($icoStream)
$writer.Write([UInt16]0)               # reserved
$writer.Write([UInt16]1)               # type: icon
$writer.Write([UInt16]$frames.Count)

$offset = 6 + (16 * $frames.Count)
foreach ($f in $frames) {
  $frameSize = $f[0]; $bytes = $f[1]
  # 256 is encoded as 0 in the single-byte width/height fields.
  $dim = [Byte]$(if ($frameSize -ge 256) { 0 } else { $frameSize })
  $writer.Write($dim)
  $writer.Write($dim)
  $writer.Write([Byte]0)               # palette count
  $writer.Write([Byte]0)               # reserved
  $writer.Write([UInt16]1)             # colour planes
  $writer.Write([UInt16]32)            # bits per pixel
  $writer.Write([UInt32]$bytes.Length)
  $writer.Write([UInt32]$offset)
  $offset += $bytes.Length
}
foreach ($f in $frames) { $writer.Write($f[1]) }
$writer.Flush()

[System.IO.File]::WriteAllBytes($Out, $icoStream.ToArray())
$writer.Dispose(); $icoStream.Dispose()

Write-Host ("Wrote {0} ({1:N0} bytes, sizes: {2})" -f $Out, (Get-Item $Out).Length, ($sizes -join ", "))
