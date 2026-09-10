#include "util/models.h"

#include <cstdio>
#include <map>

#include "util/lang.h"
#include "util/net.h"

namespace transcriptor::models {

namespace {

constexpr char kWhisperBase[] =
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/";

// Older large releases are still downloadable by name — a config carried over
// from an earlier version may point at one — but they are not offered in the
// catalog, where large-v3 and its turbo supersede them.
const std::map<std::string, std::uint64_t> kLegacyWhisperSizes = {
    {"large-v1", 3'094'600'000ULL},
    {"large-v2", 3'094'600'000ULL},
};

// A model is a regular file with something in it. `exists` was too generous:
// file_size() on a directory fails and hands back the unsigned error sentinel,
// which compares comfortably greater than zero -- so a folder entered as a
// custom model path reported itself ready and the failure surfaced much later,
// inside the inference loader, as something unrecognisable. A folder left on a
// download destination did the same and stopped the downloader replacing it.
bool has_file(const paths::fs::path& p) {
    if (p.empty()) return false;
    std::error_code ec;
    if (!paths::fs::is_regular_file(p, ec)) return false;
    const auto size = paths::fs::file_size(p, ec);
    return !ec && size > 0;
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

const std::vector<WhisperModelSpec>& whisper_catalog() {
    // Ordered smallest to largest, which is also roughest to best. Sizes are
    // the ggml .bin files on HuggingFace.
    static const std::vector<WhisperModelSpec> kCatalog = {
        {"tiny", "Tiny", 77'700'000ULL,
         "Fastest and roughest. Good for checking that audio is reaching the "
         "app; not for a meeting you need to read afterwards.",
         "En hızlı ve en kaba. Sesin uygulamaya ulaştığını denemek için iyi; "
         "sonradan okunacak bir toplantı için değil."},

        {"base", "Base", 147'900'000ULL,
         "Still quick, still rough. Usable for one clear speaker in a quiet "
         "room, and little else.",
         "Yine hızlı, yine kaba. Sessiz bir odada tek ve net bir konuşmacı "
         "için kullanılabilir, fazlası değil."},

        {"small", "Small", 487'600'000ULL,
         "The lightest model worth putting a real conversation through. Runs "
         "at a sensible speed on CPU.",
         "Gerçek bir konuşmayı verebileceğiniz en hafif model. CPU'da makul "
         "bir hızda çalışır."},

        {"medium", "Medium", 1'533'800'000ULL,
         "Clearly better on accents, crosstalk and quiet speakers. Slow "
         "without a GPU.",
         "Aksanlarda, üst üste konuşmalarda ve alçak sesli konuşmacılarda "
         "belirgin biçimde daha iyi. GPU olmadan yavaş."},

        {"large-v3-turbo", "Large v3 Turbo", 1'624'600'000ULL,
         "Recommended — close to large-v3's accuracy in a fraction of the "
         "time, and half the size.",
         "Önerilen — large-v3 doğruluğuna yakın, çok daha kısa sürede ve yarı "
         "boyutta."},

        {"large-v3", "Large v3", 3'095'000'000ULL,
         "The most accurate, and the slowest. Worth it for hard audio if you "
         "have a GPU to run it on.",
         "En doğrusu ve en yavaşı. Üzerinde çalıştıracak bir GPU'nuz varsa zor "
         "kayıtlar için değer."},
    };
    return kCatalog;
}

std::string WhisperModelSpec::note() const { return L(note_en, note_tr); }

const WhisperModelSpec* whisper_catalog_entry(const std::string& id) {
    for (const WhisperModelSpec& m : whisper_catalog()) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

paths::fs::path whisper_model_file(const WhisperModelSpec& spec) {
    return paths::models_dir() / paths::from_utf8("ggml-" + spec.id + ".bin");
}

ModelSpec whisper_spec(const std::string& model_name) {
    ModelSpec s;
    s.id = model_name;
    if (model_name.empty()) return s;          // nothing chosen -> no url
    s.label = "Whisper " + model_name;

    std::uint64_t bytes = 0;
    if (const WhisperModelSpec* m = whisper_catalog_entry(model_name)) {
        bytes = m->approx_bytes;
        s.label = "Whisper " + m->label;
    } else {
        auto it = kLegacyWhisperSizes.find(model_name);
        if (it == kLegacyWhisperSizes.end()) return s;   // unknown -> caller errors
        bytes = it->second;
    }
    s.url = std::string(kWhisperBase) + "ggml-" + model_name + ".bin";
    s.approx_bytes = bytes;
    return s;
}

std::string whisper_missing_reason(const Settings& s) {
    if (whisper_ready(s)) return {};

    // Pointed at a file by hand, and it is not there. Naming the path is the
    // only useful thing to say.
    if (!s.whisper_model_path.empty()) {
        return L("The speech model file was not found: ",
                 "Konuşma modeli dosyası bulunamadı: ") +
               paths::to_utf8(s.whisper_model_file()) +
               L(" — check the path in Settings → Advanced.",
                 " — Ayarlar → Gelişmiş'teki yolu kontrol edin.");
    }
    if (s.whisper_model.empty()) {
        return L("No speech model is selected. Open Settings → General and "
                 "choose one, then download it.",
                 "Konuşma modeli seçilmedi. Ayarlar → Genel'den birini seçip "
                 "indirin.");
    }
    const WhisperModelSpec* m = whisper_catalog_entry(s.whisper_model);
    const std::string label = m ? m->label : s.whisper_model;
    return L("The speech model \"", "\"") + label +
           L("\" has not been downloaded. Open Settings → General and download "
             "it.",
             "\" konuşma modeli indirilmemiş. Ayarlar → Genel'den indirin.");
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
    //
    // CAM++ rather than the ERes2NetV2 this used to fetch. Embedding extraction
    // is about 90% of a diarization run -- 252s of the 278s a 17-minute Turkish
    // meeting took here -- so the embedding model very nearly is the runtime.
    // Measured with eval/, each model at its own tuned threshold:
    //
    //                          Turkish   English    time
    //   ERes2NetV2 (71 MB)     26.50%     6.95%    285s / 897s
    //   CAM++      (28 MB)     26.37%     8.98%     84s / 210s
    //
    // Turkish is a tie and CAM++ is 3.4x faster, which is the trade this app
    // is for. The English column is the cost: CAM++ gives up two points of
    // diarization error rate on conversational English. See eval/README.md.
    return {"3dspeaker-campplus",
            "https://huggingface.co/csukuangfj/speaker-embedding-models/"
            "resolve/main/3dspeaker_speech_campplus_sv_zh-cn_16k-common.onnx",
            28'281'138ULL,
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

std::string ensure_whisper_model_file(const WhisperModelSpec& spec,
                                      const ProgressFn& progress,
                                      net::Canceller* cancel) {
    const paths::fs::path dest = whisper_model_file(spec);
    if (has_file(dest)) return {};
    return fetch(whisper_spec(spec.id), dest, progress, cancel);
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
