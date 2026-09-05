#include "util/models.h"

#include <cstdio>
#include <map>

#include "util/lang.h"
#include "util/net.h"

namespace transcriptor::models {

namespace {

constexpr char kWhisperBase[] =
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/";

// Sizes are approximate and only drive the progress bar.
const std::map<std::string, std::uint64_t> kWhisperSizes = {
    {"tiny",      77'700'000ULL},
    {"base",     147'900'000ULL},
    {"small",    487'600'000ULL},
    {"medium", 1'533'800'000ULL},
    {"large-v1", 3'094'600'000ULL},
    {"large-v2", 3'094'600'000ULL},
    {"large-v3", 3'095'000'000ULL},
    {"large-v3-turbo", 1'624'600'000ULL},
};

bool has_file(const paths::fs::path& p) {
    std::error_code ec;
    return !p.empty() && paths::fs::exists(p, ec) &&
           paths::fs::file_size(p, ec) > 0;
}

std::string fetch(const ModelSpec& spec, const paths::fs::path& dest,
                  const ProgressFn& progress, net::Canceller* cancel) {
    if (spec.url.empty()) {
        return L("Unknown model: ", "Bilinmeyen model: ") + spec.id +
               L(" — set the file path by hand in Settings.",
                 " — Ayarlar'dan dosya yolunu elle verin.");
    }
    if (!net::can_download()) {
        return L("No download tool (curl) was found. Fetch ",
                 "İndirme aracı (curl) bulunamadı. ") + spec.label +
               L(" by hand and point Settings at it:\n",
                 " dosyasını elle indirip Ayarlar'dan yolunu gösterin:\n") + spec.url;
    }

    const std::string prefix = lang::english() ? "Downloading " + spec.label
                                               : spec.label + " indiriliyor";
    if (progress) progress(prefix + "…", -1.0);

    auto on_bytes = [&](double frac, std::uint64_t have) {
        if (!progress) return;
        std::string msg = prefix + " (" + human_size(have);
        if (spec.approx_bytes) msg += " / " + human_size(spec.approx_bytes);
        msg += ")…";
        progress(msg, frac);
    };

    net::DownloadResult r =
        net::download(spec.url, dest, spec.approx_bytes, on_bytes, cancel);
    return r.ok ? std::string{} : r.error;
}

}  // namespace

std::string human_size(std::uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; ++u; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), v < 10 ? "%.1f %s" : "%.0f %s", v, units[u]);
    return buf;
}

ModelSpec whisper_spec(const std::string& model_name) {
    ModelSpec s;
    s.id = model_name;
    s.label = "Whisper " + model_name;
    auto it = kWhisperSizes.find(model_name);
    if (it == kWhisperSizes.end()) return s;   // unknown -> no url, caller errors
    s.url = std::string(kWhisperBase) + "ggml-" + model_name + ".bin";
    s.approx_bytes = it->second;
    return s;
}

ModelSpec segmentation_spec() {
    // pyannote's segmentation-3.0 exported to ONNX — the same model the Python
    // build used through pyannote.audio, minus the PyTorch runtime.
    return {"pyannote-segmentation-3.0",
            "https://huggingface.co/csukuangfj/sherpa-onnx-pyannote-segmentation-3-0/"
            "resolve/main/model.onnx",
            5'900'000ULL,
            L("Speaker segmentation model", "Konuşmacı bölütleme modeli")};
}

ModelSpec embedding_spec() {
    // Hosted on HuggingFace rather than the sherpa-onnx GitHub release: the
    // release assets move between tags, the HF path is stable.
    return {"3dspeaker-eres2netv2",
            "https://huggingface.co/csukuangfj/speaker-embedding-models/"
            "resolve/main/3dspeaker_speech_eres2netv2_sv_zh-cn_16k-common.onnx",
            71'441'526ULL,
            L("Speaker embedding model", "Konuşmacı ses izi modeli")};
}

const std::vector<LlmModelSpec>& llm_catalog() {
    // Q4_K_M quantizations, one file each (no split GGUFs — the downloader
    // fetches a single URL). Sizes are the real file sizes on HuggingFace.
    static const std::vector<LlmModelSpec> kCatalog = {
        {"qwen3.5-4b", "Qwen3.5 4B Instruct (Q4_K_M)", "Qwen3.5-4B-Q4_K_M.gguf",
         "https://huggingface.co/unsloth/Qwen3.5-4B-GGUF/resolve/main/"
         "Qwen3.5-4B-Q4_K_M.gguf",
         2'740'937'888ULL,
         "Recommended — the best quality/size balance for summaries.",
         "Önerilen — Türkçe özet kalitesi/boyut dengesi en iyi burada."},

        {"qwen3.5-2b", "Qwen3.5 2B Instruct (Q4_K_M)", "Qwen3.5-2B-Q4_K_M.gguf",
         "https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/"
         "Qwen3.5-2B-Q4_K_M.gguf",
         1'280'835'840ULL,
         "Light — for 4 GB of VRAM, or CPU only.",
         "Hafif — 4 GB VRAM ya da salt CPU için."},

        {"qwen3.5-0.8b", "Qwen3.5 0.8B Instruct (Q4_K_M)",
         "Qwen3.5-0.8B-Q4_K_M.gguf",
         "https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/"
         "Qwen3.5-0.8B-Q4_K_M.gguf",
         532'517'120ULL,
         "Smallest — fast on weak machines, but a rougher summary.",
         "En küçük — zayıf makinelerde hızlı, özetler daha kaba."},

        {"gemma-4-e2b", "Gemma 4 E2B Instruct (Q4_K_M)",
         "gemma-4-E2B-it-Q4_K_M.gguf",
         "https://huggingface.co/unsloth/gemma-4-E2B-it-GGUF/resolve/main/"
         "gemma-4-E2B-it-Q4_K_M.gguf",
         3'106'738'272ULL,
         "Google Gemma 4, the small edition.",
         "Google Gemma 4, küçük sürüm."},

        {"gemma-4-e4b", "Gemma 4 E4B Instruct (Q4_K_M)",
         "gemma-4-E4B-it-Q4_K_M.gguf",
         "https://huggingface.co/unsloth/gemma-4-E4B-it-GGUF/resolve/main/"
         "gemma-4-E4B-it-Q4_K_M.gguf",
         4'977'171'584ULL,
         "The large Gemma 4 — wants 8 GB of VRAM.",
         "Gemma 4'ün büyüğü — 8 GB VRAM ister."},

        {"qwen2.5-7b", "Qwen2.5 7B Instruct (Q4_K_M)",
         "Qwen2.5-7B-Instruct-Q4_K_M.gguf",
         "https://huggingface.co/bartowski/Qwen2.5-7B-Instruct-GGUF/resolve/main/"
         "Qwen2.5-7B-Instruct-Q4_K_M.gguf",
         4'683'074'240ULL,
         "The old default — heaviest of the set, wants 8 GB of VRAM.",
         "Eski varsayılan — en ağırı, 8 GB VRAM ister."},
    };
    return kCatalog;
}

std::string LlmModelSpec::note() const { return L(note_en, note_tr); }

const LlmModelSpec* llm_spec(const std::string& id) {
    for (const LlmModelSpec& m : llm_catalog()) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

paths::fs::path llm_model_file(const LlmModelSpec& spec) {
    return paths::models_dir() / paths::from_utf8(spec.filename);
}

std::string ensure_llm_model(const LlmModelSpec& spec, const ProgressFn& progress,
                             net::Canceller* cancel) {
    const paths::fs::path dest = llm_model_file(spec);
    if (has_file(dest)) return {};
    return fetch({spec.id, spec.url, spec.approx_bytes, spec.label}, dest, progress,
                 cancel);
}

bool whisper_ready(const Settings& s) { return has_file(s.whisper_model_file()); }

bool diarization_ready(const Settings& s) {
    return has_file(s.segmentation_model_file()) && has_file(s.embedding_model_file());
}

std::string ensure_whisper_model(const Settings& s, const ProgressFn& progress,
                                 net::Canceller* cancel) {
    const paths::fs::path dest = s.whisper_model_file();
    if (has_file(dest)) return {};
    if (!s.whisper_model_path.empty()) {
        // The user pointed at a specific file; don't guess a download for it.
        return L("Whisper model not found: ", "Whisper modeli bulunamadı: ") +
               paths::to_utf8(dest);
    }
    return fetch(whisper_spec(s.whisper_model), dest, progress, cancel);
}

std::string ensure_diarization_models(const Settings& s, const ProgressFn& progress,
                                      net::Canceller* cancel) {
    const paths::fs::path seg = s.segmentation_model_file();
    if (!has_file(seg)) {
        if (!s.diar_segmentation_model.empty())
            return L("Segmentation model not found: ",
                     "Bölütleme modeli bulunamadı: ") + paths::to_utf8(seg);
        std::string err = fetch(segmentation_spec(), seg, progress, cancel);
        if (!err.empty()) return err;
    }
    const paths::fs::path emb = s.embedding_model_file();
    if (!has_file(emb)) {
        if (!s.diar_embedding_model.empty())
            return L("Speaker-embedding model not found: ",
                     "Ses izi modeli bulunamadı: ") + paths::to_utf8(emb);
        std::string err = fetch(embedding_spec(), emb, progress, cancel);
        if (!err.empty()) return err;
    }
    return {};
}

}  // namespace transcriptor::models
