param(
    [switch]$SkipServer
)

$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
$buildDir = Join-Path $projectRoot "build"
$outputDir = Join-Path $projectRoot "output"
$configuration = "Release"
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

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    throw "cmake was not found. Install CMake and add it to PATH."
}

Write-Host "[1/4] Configuring the CMake build."
& $cmake.Source `
    -S $projectRoot `
    -B $buildDir `
    "-DCMAKE_BUILD_TYPE=$configuration"
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed."
}

Write-Host "[2/4] Building the C program with CMake."
& $cmake.Source `
    --build $buildDir `
    --config $configuration `
    --target c-rocket
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed."
}

$programCandidates = @(
    (Join-Path $buildDir "c-rocket.exe"),
    (Join-Path (Join-Path $buildDir $configuration) "c-rocket.exe")
)
$program = $programCandidates |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1

if (-not $program) {
    $program = Get-ChildItem -Path $buildDir -Filter "c-rocket.exe" -File -Recurse |
        Where-Object { $_.DirectoryName -notlike "*\CMakeFiles\*" } |
        Select-Object -First 1 -ExpandProperty FullName
}

if (-not $program) {
    throw "CMake built the target, but c-rocket.exe was not found in $buildDir."
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
Write-Host "[4/4] Preparing web visualization."

if ($SkipServer) {
    Write-Host "[4/4] Web server skipped by -SkipServer."
    exit 0
}

if (-not (Test-PortAvailable -Port $port)) {
    throw "localhost:$port is already in use. Stop the existing server and try again."
}

$pythonCandidates = @(
    Get-Command python.exe -All -ErrorAction SilentlyContinue
    Get-Command python3.exe -All -ErrorAction SilentlyContinue
    Get-Command py.exe -All -ErrorAction SilentlyContinue
) | Where-Object { $_.CommandType -eq "Application" }

$pythonPath = $null
foreach ($candidate in $pythonCandidates) {
    if (-not (Test-Path -LiteralPath $candidate.Source -PathType Leaf)) {
        continue
    }

    $pythonCheckExitCode = 1
    try {
        & $candidate.Source -c "import sys" *> $null
        $pythonCheckExitCode = $LASTEXITCODE
    }
    catch {
        $pythonCheckExitCode = 1
    }

    if ($pythonCheckExitCode -eq 0) {
        $pythonPath = $candidate.Source
        break
    }
}

if (-not $pythonPath) {
    throw "A working Python 3 interpreter was not found. Add Python to PATH and try again."
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
    -FilePath $pythonPath `
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
