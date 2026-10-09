# =============================================================================
# TTBOX HDMI output detect + one-click fix  (ASCII only: PowerShell 5.1
# parses UTF-8-no-BOM files as GBK and breaks on Chinese comments)
#
# Root cause (confirmed 2026-09-10):
#   When Windows desktop is NOT extended to the board's HDMI input
#   (EDID name OPI-COMPAT), the GPU still outputs valid timing
#   (board hdmirx shows Lock, 2560x1440p144) but pixel content is
#   ALL BLACK -> board captures pure-black frames (JPEG fixed ~7KB),
#   model detects nothing. Host reboot / driver reset can trigger it.
#
# Usage (git-bash on host):
#   powershell.exe -ExecutionPolicy Bypass -File core/tools/fix_hdmi_output.ps1
#
# Steps:
#   1. Check for a non-primary screen with real bounds
#   2. If missing -> DisplaySwitch.exe /extend
#   3. Capture that screen, verify it is not black (avg luminance)
#
# Exit codes: 0=OK, 1=no second screen even after extend, 2=extended but black
# =============================================================================
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

function Get-BoardScreen {
    return [System.Windows.Forms.Screen]::AllScreens |
        Where-Object { -not $_.Primary -and $_.Bounds.Width -gt 0 } |
        Select-Object -First 1
}

$t = Get-BoardScreen
if (-not $t) {
    Write-Output "[1/3] No extended screen found (desktop only on primary) -> running DisplaySwitch /extend ..."
    DisplaySwitch.exe /extend | Out-Null
    Start-Sleep -Seconds 4
    $t = Get-BoardScreen
    if (-not $t) {
        Write-Output "FAILED: still no second screen after extend. Check HDMI cable / board power / NVIDIA control panel (monitor may be disabled there)."
        exit 1
    }
} else {
    Write-Output ("[1/3] Extended screen present: {0} bounds={1}" -f $t.DeviceName, $t.Bounds)
}

# Capture the extended screen to verify content is not black
$b = $t.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
$out = Join-Path $env:USERPROFILE "Desktop\ttbox_disp2.png"
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output ("[2/3] Captured extended screen -> {0}" -f $out)

# Quick brightness check on a 64x64 downsample (full-image scan is too slow)
$img = [System.Drawing.Image]::FromFile($out)
$check = New-Object System.Drawing.Bitmap 64, 64
$cg = [System.Drawing.Graphics]::FromImage($check)
$cg.DrawImage($img, 0, 0, 64, 64)
$cg.Dispose()
$img.Dispose()
$sum = 0; $n = 0
for ($x = 0; $x -lt 64; $x += 2) {
    for ($y = 0; $y -lt 64; $y += 2) {
        $p = $check.GetPixel($x, $y)
        $sum += [int]$p.R + [int]$p.G + [int]$p.B
        $n++
    }
}
$check.Dispose()
$avg = $sum / ($n * 3)
if ($avg -lt 3) {
    Write-Output ("[3/3] WARNING: extended screen is still near-black (avg luminance {0:N1}). Desktop extended but content black - make sure some window/video is actually shown on that screen." -f $avg)
    exit 2
} else {
    Write-Output ("[3/3] OK: extended screen has content (avg luminance {0:N1}). Board preview should recover; Core auto-reopens capture within ~10s." -f $avg)
    exit 0
}
