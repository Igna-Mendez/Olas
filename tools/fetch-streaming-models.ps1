#Requires -Version 5.1
[CmdletBinding()]
param(
    [string[]]$Languages = @("en", "es"),
    [string]$Version = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# ---- TLS ----
# Old Windows PowerShell defaults to TLS 1.0. download.moonshine.ai requires
# TLS 1.2+. Without this you get "The underlying connection was closed".
[Net.ServicePointManager]::SecurityProtocol = `
    [Net.SecurityProtocolType]::Tls12 -bor `
    [Net.SecurityProtocolType]::Tls11

# ---- locate repo root ----
# This script lives in <root>/tools/, so the parent of $PSScriptRoot is the
# repo root. Fall back to $MyInvocation if $PSScriptRoot is unavailable.
$scriptDir = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$root = Split-Path -Parent $scriptDir
$modelsRoot = Join-Path $root "models"

# ---- known quantized folder names ----
# Moonshine publishes a new dated folder periodically. The script probes these
# in order and uses the first one that responds. If a download 404s, run the
# directory-listing probe manually (see bottom of this file) and add the new
# name to the top of this list.
$KnownVersions = @(
    "quantized_26_08_24",
    "quantized_26_08_21",
    "quantized_26_08_15",
    "quantized_26_08_08",
    "quantized_26_08_01",
    "quantized_26_07_25"
)

$Files = @(
    "adapter.ort",
    "cross_kv.ort",
    "decoder_kv.ort",
    "encoder.ort",
    "frontend.model.ort",
    "frontend.weights.ort",
    "streaming_config.json",
    "tokenizer.bin"
)

function Test-Url([string]$url) {
    try {
        $r = Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec 15
        return $r.StatusCode -eq 200
    } catch {
        return $false
    }
}

function Resolve-Version([string]$lang, [string]$explicit) {
    if ($explicit) { return $explicit }

    Write-Host "  probing available quantized folders..."
    foreach ($v in $KnownVersions) {
        $probe = "https://download.moonshine.ai/model/small-streaming-$lang/$v/streaming_config.json"
        Write-Host "    try $v ..." -NoNewline
        if (Test-Url $probe) {
            Write-Host " OK"
            return $v
        }
        Write-Host " 404"
    }
    throw "No valid quantized_* folder found for small-streaming-$lang. " +
          "Check https://download.moonshine.ai/model/small-streaming-$lang/ " +
          "and add the current folder name to `$KnownVersions at the top of this script."
}

function Fetch-Model([string]$lang, [string]$forcedVersion) {
    $version = Resolve-Version $lang $forcedVersion
    $base = "https://download.moonshine.ai/model/small-streaming-$lang/$version"
    $dst  = Join-Path $modelsRoot "small-streaming-$lang"

    Write-Host ""
    Write-Host "== small-streaming-$lang ==" -ForegroundColor Cyan
    Write-Host "   version: $version"
    Write-Host "   source : $base"
    Write-Host "   target : $dst"

    New-Item -ItemType Directory -Force -Path $dst | Out-Null

    foreach ($f in $Files) {
        $out = Join-Path $dst $f
        if ((Test-Path $out) -and -not $Force) {
            $sz = (Get-Item $out).Length
            Write-Host ("   {0,-22} cached  ({1,14:N0} bytes)" -f $f, $sz)
            continue
        }
        Write-Host ("   {0,-22} downloading..." -f $f) -NoNewline
        try {
            Invoke-WebRequest -Uri "$base/$f" -OutFile $out -UseBasicParsing
            $sz = (Get-Item $out).Length
            Write-Host ("`b`b`b`b`b`b`b`b`b`b`b`b`b`b {0,14:N0} bytes" -f $sz)
        } catch {
            Write-Host ""
            Write-Host "   ERROR downloading $f" -ForegroundColor Red
            Write-Host "     $($_.Exception.Message)" -ForegroundColor Red
            throw
        }
    }
}

Write-Host "Fetching Small Streaming models into $modelsRoot"
foreach ($lang in $Languages) {
    Fetch-Model $lang $Version
}

Write-Host ""
Write-Host "Verifying..." -ForegroundColor Cyan
$all_ok = $true
foreach ($lang in $Languages) {
    $dst = Join-Path $modelsRoot "small-streaming-$lang"
    foreach ($f in $Files) {
        $p = Join-Path $dst $f
        if (-not (Test-Path $p) -or (Get-Item $p).Length -eq 0) {
            Write-Host "  MISSING or empty: $p" -ForegroundColor Red
            $all_ok = $false
        }
    }
}

Write-Host ""
if ($all_ok) {
    Write-Host "All model files present." -ForegroundColor Green
    Write-Host ""
    Write-Host "Run the app with:"
    Write-Host "    cd `"$root`""
    Write-Host "    .\olas_win.exe -l $($Languages -join ',') -v"
} else {
    Write-Host "Some files are missing. Re-run with -Force to redownload." -ForegroundColor Yellow
    exit 1
}