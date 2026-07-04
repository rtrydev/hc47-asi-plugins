# Build the HC47 ASI plugins natively on Windows.
#
# Prerequisites (one-time):
#   winget install MSYS2.MSYS2
#   C:\msys64\usr\bin\pacman.exe -S --noconfirm mingw-w64-i686-gcc make
#   pip install capstone pefile
#
# Usage: .\build.ps1 [-Translate] [-Install]
#   -Translate   also regenerate the dist\*.x87 patch files from the game
#                binaries (requires the game installed; ~2 minutes)
#   -Install     run install.sh via Git Bash afterwards
param([switch]$Translate, [switch]$Install)
$ErrorActionPreference = "Stop"

$msys = "C:\msys64"
if (-not (Test-Path "$msys\mingw32\bin\gcc.exe")) {
    throw "mingw-w64 i686 gcc not found — see the prerequisites in this script"
}
$env:PATH = "$msys\mingw32\bin;$msys\usr\bin;$env:PATH"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location "$here\runtime"
try {
    make
    if ($LASTEXITCODE -ne 0) { throw "make failed" }
} finally {
    Pop-Location
}

if ($Translate) {
    foreach ($m in @("HitmanDlc.dlc", "EngineData.dll", "gsc.dll",
                     "RenderD3D.dll", "RenderOpenGL.dll", "Sound.dll",
                     "System.dll")) {
        python "$here\tools\translate.py" $m
        if ($LASTEXITCODE -ne 0) { throw "translate.py $m failed" }
    }
}

if ($Install) {
    & "$msys\usr\bin\bash.exe" "$here\install.sh"
    if ($LASTEXITCODE -ne 0) { throw "install.sh failed" }
}

Write-Host "done — plugins in $here\dist"
