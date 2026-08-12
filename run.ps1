param(
    [switch]$SkipServer
)

$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
$sourceFile = Join-Path $projectRoot "src\main.c"
$buildDir = Join-Path $projectRoot "build"
$outputDir = Join-Path $projectRoot "output"
$program = Join-Path $buildDir "c-rocket.exe"
$port = 8000

function Assert-Check {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw "CSV validation failed: $Message"
    }
}

function Test-PortAvailable {
    param([int]$Port)

    $listener = [System.Net.Sockets.TcpListener]::new(
        [System.Net.IPAddress]::Loopback,
        $Port
    )

    try {
        $listener.Start()
        return $true
    }
    catch {
        return $false
    }
    finally {
        $listener.Stop()
    }
}

New-Item -ItemType Directory -Force -Path $buildDir, $outputDir | Out-Null

$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $gcc) {
    throw "gcc was not found. Add MinGW GCC to PATH and try again."
}

Write-Host "[1/4] Compiling the C program."
& $gcc.Source -std=c11 -Wall -Wextra -Wpedantic $sourceFile -o $program
if ($LASTEXITCODE -ne 0) {
    throw "C compilation failed."
}

Write-Host "[2/4] Running conversion boundary tests."
& $program --test
if ($LASTEXITCODE -ne 0) {
    throw "Conversion tests failed."
}

Write-Host "[3/4] Running UNSAFE and SAFE simulations."
Push-Location $projectRoot
try {
    & $program
    if ($LASTEXITCODE -ne 0) {
        throw "Simulation failed."
    }
}
finally {
    Pop-Location
}

$unsafe = Import-Csv (Join-Path $outputDir "unsafe.csv")
$safe = Import-Csv (Join-Path $outputDir "safe.csv")

Assert-Check ($unsafe.Count -eq 61) "UNSAFE must contain 61 data rows."
Assert-Check ($safe.Count -eq 61) "SAFE must contain 61 data rows."

$unsafe34 = $unsafe | Where-Object { $_.time -eq "34" }
$safe34 = $safe | Where-Object { $_.time -eq "34" }
$unsafe35 = $unsafe | Where-Object { $_.time -eq "35" }
$safe35 = $safe | Where-Object { $_.time -eq "35" }
$unsafe50 = $unsafe | Where-Object { $_.time -eq "50" }
$safe60 = $safe | Where-Object { $_.time -eq "60" }

for ($index = 0; $index -le 34; $index++) {
    Assert-Check (
        $unsafe[$index].x -eq $safe[$index].x -and
        $unsafe[$index].altitude -eq $safe[$index].altitude -and
        $unsafe[$index].angle -eq $safe[$index].angle
    ) "Both modes must match through T+34."
}

Assert-Check (
    $unsafe34.raw_bias -eq "32640.00" -and
    $unsafe34.converted_bias -eq "32640"
) "T+34 must remain inside the int16_t range."

Assert-Check (
    $unsafe35.status -eq "FAILED" -and
    $unsafe35.sensor_valid -eq "1" -and
    $unsafe35.sri_failed -eq "1" -and
    $unsafe35.raw_bias -eq "33600.00" -and
    $unsafe35.converted_bias -eq "" -and
    $unsafe35.control -eq "70.00"
) "The UNSAFE failure at T+35 is not correct."

Assert-Check (
    $safe35.status -eq "SAFE_MODE" -and
    $safe35.sensor_valid -eq "0" -and
    $safe35.sri_failed -eq "1" -and
    $safe35.raw_bias -eq "33600.00" -and
    $safe35.converted_bias -eq "" -and
    $safe35.control -eq "0.00"
) "The SAFE fallback at T+35 is not correct."

Assert-Check (
    $unsafe50.altitude -eq "0.00" -and
    $unsafe50.status -eq "FAILED"
) "UNSAFE must reach the ground at T+50."
Assert-Check (
    [double]::Parse(
        $safe60.altitude,
        [System.Globalization.CultureInfo]::InvariantCulture
    ) -gt 0.0 -and
    $safe60.status -eq "SAFE_MODE"
) "SAFE must keep a positive altitude at T+60."

Write-Host "All CSV checks passed."

if ($SkipServer) {
    Write-Host "[4/4] Web server skipped by -SkipServer."
    exit 0
}

if (-not (Test-PortAvailable -Port $port)) {
    throw "localhost:$port is already in use. Stop the existing server and try again."
}

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    throw "python was not found. Add Python to PATH and try again."
}

Write-Host "[4/4] Starting the web server."
$serverArguments = @(
    "-m",
    "http.server",
    "$port",
    "--bind",
    "127.0.0.1",
    "--directory",
    "`"$projectRoot`""
)

$server = Start-Process `
    -FilePath $python.Source `
    -ArgumentList $serverArguments `
    -WorkingDirectory $projectRoot `
    -WindowStyle Hidden `
    -PassThru

try {
    Start-Sleep -Milliseconds 700
    $server.Refresh()

    if ($server.HasExited) {
        throw "The web server exited during startup."
    }

    $url = "http://localhost:$port/web/"
    Start-Process $url
    Write-Host "Opened $url in the browser."
    Read-Host "Press Enter to stop the server"
}
finally {
    if (-not $server.HasExited) {
        Stop-Process -Id $server.Id
    }
}
