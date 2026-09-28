$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$models = Join-Path $root "models"

function Fetch-Model($lang, $version) {
    $base = "https://download.moonshine.ai/model/small-streaming-$lang/$version"
    $dst  = Join-Path $models "small-streaming-$lang"
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    $files = @(
        "adapter.ort","cross_kv.ort","decoder_kv.ort","encoder.ort",
        "frontend.model.ort","frontend.weights.ort",
        "streaming_config.json","tokenizer.bin"
    )
    foreach ($f in $files) {
        $out = Join-Path $dst $f
        if (Test-Path $out) { Write-Host "  $lang/$f (cached)"; continue }
        Write-Host "  $lang/$f"
        Invoke-WebRequest "$base/$f" -OutFile $out
    }
}

Write-Host "Fetching streaming models into $models"
Fetch-Model en "quantized_26_08_21"
Fetch-Model es "quantized_26_08_24"
Write-Host "Done."
