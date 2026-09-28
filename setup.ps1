# OLAS — Open Local Audio Scribe (Windows) — portable setup script
#
# Downloads Moonshine runtime + ONNX Runtime + language models into a
# self-contained folder. No admin rights needed. Idempotent: re-run to
# repair or update.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File setup.ps1
#   powershell -ExecutionPolicy Bypass -File setup.ps1 -InstallDir D:\OLAS
#   powershell -ExecutionPolicy Bypass -File setup.ps1 -Languages en,es,ja

[CmdletBinding()]
param(
    [string]$InstallDir = "$env:LOCALAPPDATA\OLAS",
    [string[]]$Languages = @("en", "es"),
    [string]$MoonshineVersion = "latest",
    [string]$OnnxRuntimeVersion = "1.24.4",
    [switch]$SkipModels,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "Continue"

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

$MoonshineUrl = if ($MoonshineVersion -eq "latest") {
    "https://github.com/moonshine-ai/moonshine/releases/latest/download/moonshine-voice-windows-x86_64.tar.gz"
} else {
    "https://github.com/moonshine-ai/moonshine/releases/download/$MoonshineVersion/moonshine-voice-windows-x86_64.tar.gz"
}

$OnnxUrl = "https://github.com/microsoft/onnxruntime/releases/download/v$OnnxRuntimeVersion/onnxruntime-win-x64-$OnnxRuntimeVersion.zip"

# Per-language model download base. Moonshine publishes quantized .ort models at
# a stable CDN path.
function Get-ModelBase([string]$lang) {
    return "https://download.moonshine.ai/model/base-$lang/quantized/base-$lang"
}

# Model components (non-streaming Base architecture).
$ModelFiles = @("encoder_model.ort", "decoder_model_merged.ort", "tokenizer.bin")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Write-Step([string]$msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Cyan
}

function Write-Ok([string]$msg) {
    Write-Host "    [ok] $msg" -ForegroundColor Green
}

function Write-Warn([string]$msg) {
    Write-Host "    [warn] $msg" -ForegroundColor Yellow
}

function Download-File([string]$url, [string]$dest) {
    Write-Host "    downloading $url"
    $tmp = "$dest.part"
    if (Test-Path $tmp) { Remove-Item $tmp -Force }
    try {
        Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing -ErrorAction Stop
        Move-Item $tmp $dest -Force
    } catch {
        if (Test-Path $tmp) { Remove-Item $tmp -Force }
        throw "download failed: $url`n  $($_.Exception.Message)"
    }
}

function Expand-TarGz([string]$archive, [string]$destDir) {
    # Windows 10 1803+ ships bsdtar as tar.exe.
    $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
    if (-not $tar) {
        throw "tar.exe not found; Windows 10 1803 or newer is required."
    }
    New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    & $tar.Source -xzf $archive -C $destDir
    if ($LASTEXITCODE -ne 0) { throw "tar extraction failed for $archive" }
}

function Expand-Zip([string]$archive, [string]$destDir) {
    New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    Expand-Archive -Path $archive -DestinationPath $destDir -Force
}

function Find-Tool([string]$name) {
    return (Get-Command $name -ErrorAction SilentlyContinue)
}

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------

Write-Step "Checking prerequisites"

if ($PSVersionTable.PSVersion.Major -lt 5) {
    throw "PowerShell 5.1 or newer is required."
}

$arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
if ($arch -ne "X64") {
    Write-Warn "Windows $arch detected; only x64 binaries are published."
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$InstallDir = (Resolve-Path $InstallDir).Path
Write-Ok "install dir: $InstallDir"

$CacheDir = Join-Path $InstallDir ".cache"
New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null

# ---------------------------------------------------------------------------
# 1. Moonshine runtime library
# ---------------------------------------------------------------------------

Write-Step "Installing Moonshine runtime"

$moonshineDir = Join-Path $InstallDir "moonshine-voice"
$moonshineTarball = Join-Path $CacheDir "moonshine-voice-windows-x86_64.tar.gz"

if ((Test-Path $moonshineDir) -and -not $Force) {
    Write-Ok "moonshine-voice already present (use -Force to reinstall)"
} else {
    if (-not (Test-Path $moonshineTarball) -or $Force) {
        Download-File $MoonshineUrl $moonshineTarball
    }
    if (Test-Path $moonshineDir) { Remove-Item $moonshineDir -Recurse -Force }
    Expand-TarGz $moonshineTarball $InstallDir
    if (Test-Path $moonshineDir) {
        Write-Ok "moonshine-voice extracted"
    } else {
        # Tarball may extract with a versioned folder name; find and rename.
        $candidates = Get-ChildItem $InstallDir -Directory |
            Where-Object { $_.Name -like "moonshine*" -and $_.Name -ne "moonshine-voice" }
        if ($candidates) {
            Rename-Item $candidates[0].FullName $moonshineDir
            Write-Ok "moonshine-voice renamed from $($candidates[0].Name)"
        } else {
            throw "moonshine-voice directory not found after extraction"
        }
    }
}

# ---------------------------------------------------------------------------
# 2. ONNX Runtime
# ---------------------------------------------------------------------------

Write-Step "Installing ONNX Runtime $OnnxRuntimeVersion"

$onnxDir = Join-Path $InstallDir "onnxruntime"
$onnxZip = Join-Path $CacheDir "onnxruntime-win-x64-$OnnxRuntimeVersion.zip"

if ((Test-Path $onnxDir) -and -not $Force) {
    Write-Ok "onnxruntime already present (use -Force to reinstall)"
} else {
    if (-not (Test-Path $onnxZip) -or $Force) {
        Download-File $OnnxUrl $onnxZip
    }
    $tmpExtract = Join-Path $CacheDir "onnx-extract"
    if (Test-Path $tmpExtract) { Remove-Item $tmpExtract -Recurse -Force }
    Expand-Zip $onnxZip $tmpExtract

    # The zip contains onnxruntime-win-x64-<version>/ with lib/ and include/.
    $inner = Get-ChildItem $tmpExtract -Directory | Select-Object -First 1
    if (-not $inner) { throw "unexpected onnxruntime zip layout" }

    if (Test-Path $onnxDir) { Remove-Item $onnxDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $onnxDir | Out-Null
    Copy-Item (Join-Path $inner.FullName "lib\*")       $onnxDir -Recurse -Force
    Copy-Item (Join-Path $inner.FullName "include\*")   $onnxDir -Recurse -Force
    Remove-Item $tmpExtract -Recurse -Force
    Write-Ok "onnxruntime installed"
}

# ---------------------------------------------------------------------------
# 3. Language models
# ---------------------------------------------------------------------------

if ($SkipModels) {
    Write-Step "Skipping model download (-SkipModels)"
} else {
    $modelsDir = Join-Path $InstallDir "models"
    New-Item -ItemType Directory -Force -Path $modelsDir | Out-Null

    foreach ($lang in $Languages) {
        Write-Step "Installing model base-$lang"
        $langDir = Join-Path $modelsDir "base-$lang"
        $baseUrl = Get-ModelBase $lang

        # Validate language against the known set to fail fast.
        if ($lang -notmatch '^(en|es|ar|ja|ko|zh|vi|uk)$') {
            Write-Warn "unknown language '$lang'; skipping"
            continue
        }

        New-Item -ItemType Directory -Force -Path $langDir | Out-Null

        $missing = @()
        foreach ($f in $ModelFiles) {
            if (-not (Test-Path (Join-Path $langDir $f)) -or $Force) {
                $missing += $f
            }
        }

        if ($missing.Count -eq 0) {
            Write-Ok "base-$lang already complete"
            continue
        }

        foreach ($f in $missing) {
            $dest = Join-Path $langDir $f
            Download-File "$baseUrl/$f" $dest
            $size = (Get-Item $dest).Length
            Write-Ok "base-$lang/$f ($([math]::Round($size/1MB,1)) MB)"
        }

        # Sanity check: tokenizer.bin is tiny but must be present.
        if (-not (Test-Path (Join-Path $langDir "tokenizer.bin"))) {
            Write-Warn "base-$lang is missing tokenizer.bin — model may not load"
        }
    }
}

# ---------------------------------------------------------------------------
# 4. Launcher
# ---------------------------------------------------------------------------

Write-Step "Writing launcher"

$exe = Join-Path $InstallDir "olas_win.exe"
$launcher = Join-Path $InstallDir "OLAS.bat"

$modelArgs = ($Languages | ForEach-Object { "models\base-$_" }) -join ","
$langArgs  = ($Languages -join ",")

$bat = @"
@echo off
setlocal
cd /d "%~dp0"
set PATH=%~dp0onnxruntime;%PATH%
"%~dp0olas_win.exe" -l $langArgs -m "$modelArgs"
endlocal
"@

Set-Content -Path $launcher -Value $bat -Encoding ASCII
Write-Ok "launcher: $launcher"

# ---------------------------------------------------------------------------
# 5. Desktop shortcut (best-effort, no admin)
# ---------------------------------------------------------------------------

Write-Step "Creating desktop shortcut"

try {
    $ws = New-Object -ComObject WScript.Shell
    $lnk = $ws.CreateShortcut((Join-Path ([Environment]::GetFolderPath("Desktop")) "OLAS.lnk"))
    $lnk.TargetPath       = $launcher
    $lnk.WorkingDirectory = $InstallDir
    $lnk.IconLocation     = "$exe,0"
    $lnk.Description      = "OLAS — Open Local Audio Scribe"
    $lnk.Save()
    Write-Ok "desktop shortcut created"
} catch {
    Write-Warn "could not create desktop shortcut: $($_.Exception.Message)"
}

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------

Write-Host ""
Write-Host "OLAS is installed in: $InstallDir" -ForegroundColor Green
Write-Host ""
Write-Host "Run it by double-clicking OLAS.bat or the desktop shortcut." -ForegroundColor Green
Write-Host ""
if (Test-Path $exe) {
    Write-Host "Main executable: $exe"
} else {
    Write-Warn "olas_win.exe is not present yet."
    Write-Warn "Place your compiled olas_win.exe next to this script and re-run,"
    Write-Warn "or build it per README.md and copy it into $InstallDir."
}