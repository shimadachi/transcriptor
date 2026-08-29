#!/usr/bin/env bash
# Pre-download the model weights the app would otherwise fetch on first run.
#
# Useful for building an offline installer, or when the machine that runs the
# app has no internet. Nothing here is required — the app downloads what it
# needs by itself.
#
#   ./scripts/fetch_models.sh [whisper-model] [target-dir]
#
# whisper-model defaults to large-v3; pass tiny/base/small/medium for a smaller
# download. target-dir defaults to the app's models directory.

set -euo pipefail

MODEL="${1:-large-v3}"

default_models_dir() {
    case "$(uname -s)" in
        Darwin) echo "$HOME/Library/Application Support/Transcriptor/models" ;;
        *)      echo "${XDG_CONFIG_HOME:-$HOME/.config}/Transcriptor/models" ;;
    esac
}

DEST="${2:-$(default_models_dir)}"
mkdir -p "$DEST/diarize"

fetch() {
    local url="$1" out="$2"
    if [ -s "$out" ]; then
        echo "  var, atlanıyor: $(basename "$out")"
        return
    fi
    echo "  indiriliyor: $(basename "$out")"
    curl -fL --retry 3 --progress-bar -o "$out.part" "$url"
    mv "$out.part" "$out"
}

echo "Hedef: $DEST"
echo
echo "Whisper ($MODEL):"
fetch "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-${MODEL}.bin" \
      "$DEST/ggml-${MODEL}.bin"

echo
echo "Konuşmacı ayrımı:"
fetch "https://huggingface.co/csukuangfj/sherpa-onnx-pyannote-segmentation-3-0/resolve/main/model.onnx" \
      "$DEST/diarize/segmentation.onnx"
fetch "https://huggingface.co/csukuangfj/speaker-embedding-models/resolve/main/3dspeaker_speech_eres2netv2_sv_zh-cn_16k-common.onnx" \
      "$DEST/diarize/speaker-embedding.onnx"

echo
echo "Bitti. Özetleyici modelini uygulamadan indirebilirsiniz:"
echo "  Ayarlar → Özetleyici → Hazır model indir"
echo "Elle indirmek isterseniz .gguf dosyasını şuraya koyun: $DEST"
echo "Örnek (Qwen3.5 4B Instruct, ~2.6 GB):"
echo "  https://huggingface.co/unsloth/Qwen3.5-4B-GGUF"
