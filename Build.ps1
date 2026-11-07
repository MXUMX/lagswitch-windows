param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
cmake -S . -B build -A x64
if ($LASTEXITCODE) { throw 'CMake configuration failed' }
cmake --build build --config Release --parallel
if ($LASTEXITCODE) { throw 'Build failed' }
if (-not $SkipTests) {
    # The integration test uses only ephemeral loopback UDP sockets. It needs
    # elevation for WFP, never installs a system-wide test filter.
    ctest --test-dir build -C Release --output-on-failure
    if ($LASTEXITCODE) { throw 'Tests failed. Run this script as administrator for the isolated WFP test.' }
}
cmake --install build --config Release --prefix dist
if ($LASTEXITCODE) { throw 'Packaging failed' }
Get-FileHash dist/LagSwitch.exe -Algorithm SHA256
Write-Host 'Ready: dist/LagSwitch.exe'
