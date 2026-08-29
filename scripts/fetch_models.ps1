# Pre-download the model weights the app would otherwise fetch on first run.
#
# Useful for building an offline installer, or when the machine that runs the
# app has no internet. Nothing here is required — the app downloads what it
# needs by itself.
#
#   .\scripts\fetch_models.ps1 [-Model large-v3] [-Dest <path>]

param(
    [string]$Model = "large-v3",
    [string]$Dest  = "$env:APPDATA\Transcriptor\models"
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path $Dest            | Out-Null
New-Item -ItemType Directory -Force -Path "$Dest\diarize"  | Out-Null

function Fetch($Url, $Out) {
    if ((Test-Path $Out) -and ((Get-Item $Out).Length -gt 0)) {
        Write-Host "  var, atlaniyor: $(Split-Path $Out -Leaf)"
        return
    }
    Write-Host "  indiriliyor: $(Split-Path $Out -Leaf)"
    # Invoke-WebRequest's progress bar makes large downloads crawl; suppress it.
    $prev = $ProgressPreference
    $ProgressPreference = "SilentlyContinue"
    try {
        Invoke-WebRequest -Uri $Url -OutFile "$Out.part" -UseBasicParsing
        Move-Item -Force "$Out.part" $Out
    } finally {
        $ProgressPreference = $prev
    }
}

Write-Host "Hedef: $Dest"
Write-Host ""
Write-Host "Whisper ($Model):"
Fetch "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$Model.bin" `
      "$Dest\ggml-$Model.bin"

Write-Host ""
Write-Host "Konusmaci ayrimi:"
Fetch "https://huggingface.co/csukuangfj/sherpa-onnx-pyannote-segmentation-3-0/resolve/main/model.onnx" `
      "$Dest\diarize\segmentation.onnx"
Fetch "https://huggingface.co/csukuangfj/speaker-embedding-models/resolve/main/3dspeaker_speech_eres2netv2_sv_zh-cn_16k-common.onnx" `
      "$Dest\diarize\speaker-embedding.onnx"

Write-Host ""
Write-Host "Bitti. Ozetleyici modelini uygulamadan indirebilirsiniz:"
Write-Host "  Ayarlar -> Ozetleyici -> Hazir model indir"
Write-Host "Elle indirmek isterseniz .gguf dosyasini suraya koyun: $Dest"
Write-Host "Ornek (Qwen3.5 4B Instruct, ~2.6 GB):"
Write-Host "  https://huggingface.co/unsloth/Qwen3.5-4B-GGUF"
