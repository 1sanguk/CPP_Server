param([switch]$NoLaunch)

$ErrorActionPreference = "Stop"

$repoRoot = $PSScriptRoot
$serverBuildDir = Join-Path $repoRoot "build/server"
$labBuildDir = Join-Path $repoRoot "build/server_lab"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "CMake was not found. Installing CMake..."
    $winget = Get-Command winget -ErrorAction SilentlyContinue
    $choco = Get-Command choco -ErrorAction SilentlyContinue
    if ($winget) {
        & $winget.Source install --id Kitware.CMake -e --source winget --accept-package-agreements --accept-source-agreements
    } elseif ($choco) {
        & $choco.Source install cmake -y
    } else {
        throw "CMake is required, and no supported package manager (winget or choco) was found to install it automatically."
    }

    $cmakeBin = Join-Path ${env:ProgramFiles} "CMake\bin"
    if (Test-Path $cmakeBin) {
        $env:PATH = "$cmakeBin;$env:PATH"
    }
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        throw "CMake installation finished, but 'cmake' is still not on PATH. Restart your terminal and run .\start.ps1 again."
    }
}

$qtPrefix = $env:SERVER_LAB_QT_PREFIX
if (-not $qtPrefix) {
    foreach ($tool in @("qmake6", "qmake")) {
        $command = Get-Command $tool -ErrorAction SilentlyContinue
        if ($command) {
            $qtPrefix = & $command.Source -query QT_INSTALL_PREFIX
            if ($qtPrefix) { break }
        }
    }
}

if (-not $qtPrefix) {
    Write-Host "Qt Widgets was not found. Installing Qt 6 into the repository tool directory..."
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) {
        $python = Get-Command py -ErrorAction SilentlyContinue
    }
    if (-not $python) {
        throw "Python is required for automatic Qt installation on Windows."
    }

    & $python.Source -m pip install --user aqtinstall
    $qtRoot = Join-Path $repoRoot ".tools/Qt"
    & $python.Source -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --outputdir $qtRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Automatic Qt installation failed."
    }
    $qtPrefix = Join-Path $qtRoot "6.8.3/msvc2022_64"
}

Write-Host "[1/5] Configuring Windows server versions..."
cmake -S (Join-Path $repoRoot "server") -B $serverBuildDir -A x64
if ($LASTEXITCODE -ne 0) {
    throw "Configuring Windows server versions failed."
}

Write-Host "[2/5] Building Windows server versions..."
cmake --build $serverBuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Building Windows server versions failed."
}

Write-Host "[3/5] Configuring Server Architecture Lab..."
$configureArgs = @("-S", (Join-Path $repoRoot "server_lab"), "-B", $labBuildDir, "-A", "x64")
if ($qtPrefix) {
    $configureArgs += "-DCMAKE_PREFIX_PATH=$qtPrefix"
}
& cmake $configureArgs
if ($LASTEXITCODE -ne 0) {
    throw "Qt Widgets development files were not found. Install Qt 6 or set SERVER_LAB_QT_PREFIX."
}

Write-Host "[4/5] Building and testing Server Architecture Lab..."
cmake --build $labBuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Building Server Architecture Lab failed."
}
ctest --test-dir $labBuildDir -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) {
    throw "Server Architecture Lab tests failed."
}
$labExecutable = Join-Path $labBuildDir "Release/MMO Server Lab.exe"
$deployTool = Join-Path $qtPrefix "bin/windeployqt.exe"
if (Test-Path $deployTool) {
    & $deployTool --release --no-translations $labExecutable
    if ($LASTEXITCODE -ne 0) {
        throw "windeployqt failed."
    }
}

Write-Host "[5/5] Starting Server Architecture Lab..."
if ($NoLaunch) {
    Write-Host "Build and tests completed (-NoLaunch)."
    exit 0
}
Set-Location $repoRoot
Start-Process $labExecutable
