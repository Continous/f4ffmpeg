[CmdletBinding()]
param(
    [string]$Triplet = "x64-windows-static-md"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$OverlayRoot = Join-Path $ProjectRoot "vcpkg-overlays"

if (-not $env:VCPKG_ROOT) {
    if (Test-Path "C:\vcpkg\vcpkg.exe") {
        $env:VCPKG_ROOT = "C:\vcpkg"
    } else {
        throw "VCPKG_ROOT is not set and C:\vcpkg\vcpkg.exe was not found."
    }
}

$Vcpkg = Join-Path $env:VCPKG_ROOT "vcpkg.exe"
if (-not (Test-Path $Vcpkg)) {
    throw "vcpkg.exe was not found at $Vcpkg"
}

$clang = Get-Command clang-cl.exe -ErrorAction SilentlyContinue
if (-not $clang) {
    $clang = Get-Command clang-cl -ErrorAction SilentlyContinue
}

if (-not $clang) {
    Write-Host "clang-cl is not currently on PATH; the overlay port will also check Visual Studio's LLVM directories."
} else {
    Write-Host "clang-cl: $($clang.Source)"
}

$env:VCPKG_OVERLAY_PORTS = $OverlayRoot
$env:VCPKG_DEFAULT_TRIPLET = $Triplet
$env:VCPKG_DEFAULT_HOST_TRIPLET = "x64-windows"

Write-Host "Installing libplacebo via vcpkg overlay..."
Write-Host "  VCPKG_ROOT: $env:VCPKG_ROOT"
Write-Host "  overlay:    $OverlayRoot"
Write-Host "  triplet:    $Triplet"

& $Vcpkg install "libplacebo:$Triplet" "--overlay-ports=$OverlayRoot"
if ($LASTEXITCODE -ne 0) {
    throw "vcpkg failed to install libplacebo."
}
