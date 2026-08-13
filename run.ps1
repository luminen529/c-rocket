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

$unsafe36 = $unsafe | Where-Object { $_.time -eq "36" }
$unsafe37 = $unsafe | Where-Object { $_.time -eq "37" }
$unsafe38 = $unsafe | Where-Object { $_.time -eq "38" }
$unsafe39 = $unsafe | Where-Object { $_.time -eq "39" }
$unsafe40 = $unsafe | Where-Object { $_.time -eq "40" }
$safe37 = $safe | Where-Object { $_.time -eq "37" }
$safe60 = $safe | Where-Object { $_.time -eq "60" }

for ($index = 0; $index -le 36; $index++) {
    Assert-Check (
        $unsafe[$index].x -eq $safe[$index].x -and
        $unsafe[$index].altitude -eq $safe[$index].altitude -and
        $unsafe[$index].angle -eq $safe[$index].angle
    ) "Both modes must match through T+36."
}

Assert-Check (
    $unsafe36.raw_bias -eq "32400.00" -and
    $unsafe36.converted_bias -eq "32400" -and
    $unsafe36.conversion_result -eq "OK" -and
    $unsafe36.sri1_status -eq "RUNNING" -and
    $unsafe36.sri2_status -eq "RUNNING"
) "T+36 must remain inside the int16_t range with both SRIs running."

Assert-Check (
    $unsafe37.status -eq "CONTROL_LOST" -and
    $unsafe37.raw_bias -eq "33300.00" -and
    $unsafe37.converted_bias -eq "" -and
    $unsafe37.conversion_result -eq "OPERAND_ERROR" -and
    $unsafe37.sri1_status -eq "STOPPED" -and
    $unsafe37.sri2_status -eq "STOPPED" -and
    $unsafe37.sri1_failure_time -eq "36.928" -and
    $unsafe37.sri2_failure_time -eq "37.000" -and
    $unsafe37.obc_input -eq "DIAGNOSTIC" -and
    $unsafe37.nozzle_command -eq "FULL_DEFLECTION"
) "The two-SRI UNSAFE failure sequence at T+37 is not correct."

Assert-Check (
    $unsafe38.status -eq "CONTROL_LOST" -and
    $unsafe38.angle -eq "12.00" -and
    $unsafe39.status -eq "FAILED" -and
    $unsafe39.angle -eq "24.00" -and
    [double]::Parse(
        $unsafe39.altitude,
        [System.Globalization.CultureInfo]::InvariantCulture
    ) -gt 0.0
) "UNSAFE must lose control before breaking up at T+39."

Assert-Check (
    $unsafe40.x -eq $unsafe39.x -and
    $unsafe40.altitude -eq $unsafe39.altitude -and
    $unsafe40.angle -eq $unsafe39.angle -and
    $unsafe40.status -eq "FAILED"
) "UNSAFE must remain at the breakup position instead of falling to ground."

Assert-Check (
    ($safe | Where-Object { $_.conversion_result -ne "NOT_RUN" }).Count -eq 0 -and
    ($safe | Where-Object { $_.sri1_status -ne "RUNNING" }).Count -eq 0 -and
    ($safe | Where-Object { $_.sri2_status -ne "RUNNING" }).Count -eq 0 -and
    ($safe | Where-Object { $_.status -ne "NORMAL" }).Count -eq 0 -and
    $safe37.obc_input -eq "FLIGHT" -and
    $safe37.nozzle_command -eq "NEUTRAL" -and
    [double]::Parse(
        $safe60.altitude,
        [System.Globalization.CultureInfo]::InvariantCulture
    ) -gt 0.0
) "SAFE must keep alignment off and remain healthy through T+60."

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
