$ErrorActionPreference = "Stop"

$vsCandidates = @(
  "E:\.trae\Microsoft Visual Studio 12.0\VC\vcvarsall.bat",
  "C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat",
  "D:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat"
)

$vcvars = $vsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) {
  Write-Host "[ERROR] MSVC 2013 not found (vcvarsall.bat missing)."
  Write-Host "Install Visual Studio 2013 (VC++) or MSVC 2013 Build Tools (x64)."
  exit 1
}

$qtBin = $env:QT_BIN
if (-not $qtBin) {
  $qtCandidates = @(
    "E:\.trae\Qt\5.4\msvc2013_64\bin",
    "E:\Qt\5.4\msvc2013_64\bin",
    "C:\Qt\Qt5.4.2\5.4\msvc2013_64\bin"
  )
  $qtBin = $qtCandidates | Where-Object { Test-Path (Join-Path $_ "qmake.exe") } | Select-Object -First 1
}

$qmake = Join-Path $qtBin "qmake.exe"
if (-not (Test-Path $qmake)) {
  Write-Host "[ERROR] qmake.exe not found under QT_BIN: $qtBin"
  exit 1
}

Write-Host "[OK] vcvarsall.bat: $vcvars"
Write-Host "[OK] qmake.exe: $qmake"
Write-Host "Next: run win_setup_test\build_msvc2013.bat"
