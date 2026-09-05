#include "diarize/diarizer.h"
#include "util/cpu.h"
#include "util/lang.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <thread>

#ifdef TRANSCRIPTOR_HAVE_DIARIZE
// The C API, not the C++ one: sherpa-onnx's cxx-api does not cover speaker
// diarization, and the C surface is the stable one across releases.
#  include "sherpa-onnx/c-api/c-api.h"
#endif

namespace transcriptor::diarize {

std::string speaker_name(int speaker, const std::string& lang) {
    const bool tr = (lang == "tr");
    if (speaker < 0) return tr ? "Konuşmacı ?" : "Speaker ?";
    return (tr ? "Konuşmacı " : "Speaker ") + std::to_string(speaker + 1);
}

bool Diarizer::supported() {
#ifdef TRANSCRIPTOR_HAVE_DIARIZE
    return true;
#else
    return false;
#endif
}

#ifdef TRANSCRIPTOR_HAVE_DIARIZE

namespace {

// One thread per physical core. Measured on 196s of recorded speech, on a
// Ryzen 7 7735HS -- 8 physical cores, 16 logical -- process time only:
//
//    1 thread  115.5s      8 threads   29.1s   <- physical core count
//    2 threads  65.6s     10 threads   35.9s
//    4 threads  41.0s     12 threads   42.0s
//    6 threads  34.0s     16 threads   53.5s
//
// The peak sits exactly on the physical count and falls away either side, so
// the number that matters is cores, not logical processors and not a constant.
// The old cap of 4 gave away ~30% here; a constant 8 would have been the same
// mistake in the other direction, since on a 4-core/8-thread laptop it means
// running two SMT siblings per core -- the 16-thread column above.
int diar_threads() {
    return static_cast<int>(std::max(1u, util::physical_cores()));
}

struct ProgressBridge {
    const ProgressFn* fn = nullptr;
};

int32_t on_progress(int32_t processed, int32_t total, void* arg) {
    auto* bridge = static_cast<ProgressBridge*>(arg);
    if (bridge && bridge->fn && *bridge->fn && total > 0) {
        (*bridge->fn)("", static_cast<double>(processed) / total);
    }
    return 0;   // non-zero would abort
}

// The C API hands back raw pointers with matching destroy functions; these
// keep the ownership straight even when Process() throws.
struct ResultGuard {
    const SherpaOnnxOfflineSpeakerDiarizationResult* r = nullptr;
    ~ResultGuard() {
        if (r) SherpaOnnxOfflineSpeakerDiarizationDestroyResult(r);
    }
};

struct SegmentsGuard {
    const SherpaOnnxOfflineSpeakerDiarizationSegment* s = nullptr;
    ~SegmentsGuard() {
        if (s) SherpaOnnxOfflineSpeakerDiarizationDestroySegment(s);
    }
};

}  // namespace

struct Diarizer::Impl {
    const SherpaOnnxOfflineSpeakerDiarization* sd = nullptr;
    std::string seg_path;
    std::string emb_path;
    int         num_speakers = 0;
    float       threshold = 0.0f;

    ~Impl() {
        if (sd) SherpaOnnxDestroyOfflineSpeakerDiarization(sd);
    }
};

Diarizer::Diarizer(Settings settings, DeviceInfo device)
    : settings_(std::move(settings)), device_(std::move(device)),
      impl_(std::make_unique<Impl>()) {}

Diarizer::~Diarizer() = default;

void Diarizer::unload() {
    if (impl_->sd) {
        SherpaOnnxDestroyOfflineSpeakerDiarization(impl_->sd);
        impl_->sd = nullptr;
        impl_->seg_path.clear();
        impl_->emb_path.clear();
    }
}

std::vector<Turn> Diarizer::diarize(const std::vector<float>& audio, int samplerate,
                                    const ProgressFn& progress) {
    if (audio.empty()) return {};

    const std::string seg = paths::to_utf8(settings_.segmentation_model_file());
    const std::string emb = paths::to_utf8(settings_.embedding_model_file());

    // Rebuild when any model or clustering knob changed since last time.
    const bool stale = impl_->sd && (impl_->seg_path != seg ||
                                     impl_->emb_path != emb ||
                                     impl_->num_speakers != settings_.num_speakers ||
                                     impl_->threshold != settings_.cluster_threshold);
    if (stale) unload();

    if (!impl_->sd) {
        if (progress) {
            progress(L("Loading the speaker models…",
                       "Konuşmacı modelleri yükleniyor…"), -1.0);
        }

        // onnxruntime's execution providers are its own list, not ggml's: the
        // only GPU one this build ships is CUDA, so a Vulkan or Metal device
        // still diarizes on the CPU rather than asking for a provider that
        // isn't there.
        const char* provider =
            (device_.use_gpu() && device_.backend == "CUDA") ? "cuda" : "cpu";

        SherpaOnnxOfflineSpeakerDiarizationConfig config{};
        config.segmentation.pyannote.model = seg.c_str();
        config.segmentation.num_threads    = diar_threads();
        config.segmentation.debug          = 0;
        config.segmentation.provider       = provider;

        config.embedding.model       = emb.c_str();
        config.embedding.num_threads = diar_threads();
        config.embedding.debug       = 0;
        config.embedding.provider    = provider;

        // num_clusters > 0 pins the speaker count and makes threshold moot;
        // -1 lets the distance threshold decide how many there are.
        if (settings_.num_speakers > 0) {
            config.clustering.num_clusters = settings_.num_speakers;
            config.clustering.threshold    = settings_.cluster_threshold;
        } else {
            config.clustering.num_clusters = -1;
            config.clustering.threshold    = settings_.cluster_threshold;
        }

        config.min_duration_on  = 0.3f;   // drop shorter blips
        config.min_duration_off = 0.5f;   // bridge shorter same-speaker gaps

        impl_->sd = SherpaOnnxCreateOfflineSpeakerDiarization(&config);
        if (!impl_->sd) {
            throw std::runtime_error(
                L("Speaker separation could not start. Check the model files:\n",
                  "Konuşmacı ayrımı başlatılamadı. Model dosyalarını kontrol edin:\n")
                + seg + "\n" + emb);
        }
        impl_->seg_path     = seg;
        impl_->emb_path     = emb;
        impl_->num_speakers = settings_.num_speakers;
        impl_->threshold    = settings_.cluster_threshold;
    }

    const int32_t expected_rate =
        SherpaOnnxOfflineSpeakerDiarizationGetSampleRate(impl_->sd);
    if (expected_rate != samplerate) {
        throw std::runtime_error(
            L("The speaker model expects ", "Konuşmacı modeli ") +
            std::to_string(expected_rate) +
            L(" Hz, but the audio is ", " Hz bekliyor, ses ") +
            std::to_string(samplerate) + " Hz.");
    }

    if (progress) progress("", 0.0);

    ProgressBridge bridge{&progress};
    ResultGuard result;
    result.r = SherpaOnnxOfflineSpeakerDiarizationProcessWithCallback(
        impl_->sd, audio.data(), static_cast<int32_t>(audio.size()), on_progress,
        &bridge);
    if (!result.r) {
        throw std::runtime_error(L("Speaker separation produced no result.",
                                   "Konuşmacı ayrımı sonuç üretmedi."));
    }

    const int32_t count =
        SherpaOnnxOfflineSpeakerDiarizationResultGetNumSegments(result.r);

    SegmentsGuard segments;
    segments.s = SherpaOnnxOfflineSpeakerDiarizationResultSortByStartTime(result.r);
    if (!segments.s || count <= 0) return {};

    // sherpa's speaker ids are already 0-based, but renumber by first
    // appearance so "Konuşmacı 1" is whoever speaks first.
    std::map<int32_t, int> order;
    std::vector<Turn> turns;
    turns.reserve(static_cast<std::size_t>(count));

    for (int32_t i = 0; i < count; ++i) {
        const auto& s = segments.s[i];
        auto it = order.find(s.speaker);
        if (it == order.end()) {
            it = order.emplace(s.speaker, static_cast<int>(order.size())).first;
        }
        turns.push_back({static_cast<double>(s.start), static_cast<double>(s.end),
                         it->second});
    }

    if (progress) progress("", 1.0);
    return turns;
}

#else   // TRANSCRIPTOR_HAVE_DIARIZE

struct Diarizer::Impl {};

Diarizer::Diarizer(Settings settings, DeviceInfo device)
    : settings_(std::move(settings)), device_(std::move(device)),
      impl_(std::make_unique<Impl>()) {}

Diarizer::~Diarizer() = default;

void Diarizer::unload() {}

std::vector<Turn> Diarizer::diarize(const std::vector<float>&, int,
                                    const ProgressFn&) {
    throw std::runtime_error(
        L("This build was compiled without speaker separation (rebuild with "
          "-DTRANSCRIPTOR_DIARIZE=ON).",
          "Bu sürüm konuşmacı ayrımı olmadan derlendi (-DTRANSCRIPTOR_DIARIZE=ON ile "
          "yeniden derleyin)."));
}

#endif  // TRANSCRIPTOR_HAVE_DIARIZE

}  // namespace transcriptor::diarize
