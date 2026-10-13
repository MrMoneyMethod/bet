$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

function Run-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

Run-Checked -Program "cmake" -Arguments @("-S", ".", "-B", "build/windows", "-A", "x64")
Run-Checked -Program "cmake" -Arguments @("--build", "build/windows", "--config", "Release", "--parallel", "2")
Run-Checked -Program "ctest" -Arguments @("--test-dir", "build/windows", "-C", "Release", "--output-on-failure")
New-Item -ItemType Directory -Force -Path "releases" | Out-Null
Copy-Item "build/windows/Release/Edgecourt.exe" "releases/Edgecourt.exe" -Force
Write-Host "Built and tested releases/Edgecourt.exe"
Write-Host "Package it with: python scripts/package_release.py"
