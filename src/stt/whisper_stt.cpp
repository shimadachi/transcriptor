#include "stt/whisper_stt.h"
#include "util/lang.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <thread>

#include <whisper.h>

namespace transcriptor::stt {

namespace {

// whisper reports timestamps in centiseconds.
inline double cs_to_sec(int64_t cs) { return static_cast<double>(cs) / 100.0; }

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

int default_threads(int configured) {
    if (configured > 0) return configured;
    const unsigned hw = std::thread::hardware_concurrency();
    // Whisper scales poorly past ~8 threads and we want headroom for the UI.
    return static_cast<int>(std::clamp<unsigned>(hw ? hw : 4, 1u, 8u));
}

struct CallbackState {
    const ProgressFn* progress = nullptr;
    std::atomic<bool>* abort = nullptr;
};

void progress_callback(whisper_context*, whisper_state*, int progress_pct,
                       void* user_data) {
    auto* st = static_cast<CallbackState*>(user_data);
    if (st && st->progress && *st->progress) {
        (*st->progress)("", static_cast<double>(progress_pct) / 100.0);
    }
}

bool abort_callback(void* user_data) {
    auto* st = static_cast<CallbackState*>(user_data);
    return st && st->abort && st->abort->load();
}

}  // namespace

struct WhisperTranscriber::Impl {
    whisper_context* ctx = nullptr;
    std::string      loaded_path;

    ~Impl() {
        if (ctx) whisper_free(ctx);
    }
};

WhisperTranscriber::WhisperTranscriber(std::string model_name, DeviceInfo device,
                                       std::string language, int threads)
    : model_name_(std::move(model_name)), device_(std::move(device)),
      language_(std::move(language)), threads_(threads),
      impl_(std::make_unique<Impl>()) {}

WhisperTranscriber::~WhisperTranscriber() = default;

bool WhisperTranscriber::loaded() const { return impl_ && impl_->ctx != nullptr; }

void WhisperTranscriber::unload() {
    if (impl_ && impl_->ctx) {
        whisper_free(impl_->ctx);
        impl_->ctx = nullptr;
        impl_->loaded_path.clear();
    }
}

std::vector<TranscriptSegment> WhisperTranscriber::transcribe(
    const std::vector<float>& audio, const paths::fs::path& model_path,
    const ProgressFn& progress) {

    abort_.store(false);

    const std::string path_utf8 = paths::to_utf8(model_path);

    // Reload when the requested model file changed (settings edit).
    if (impl_->ctx && impl_->loaded_path != path_utf8) unload();

    if (!impl_->ctx) {
        if (progress) {
            progress(L("Loading the model (", "Model yükleniyor (") +
                         model_name_ + ")…", -1.0);
        }

        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = device_.use_gpu();

        impl_->ctx = whisper_init_from_file_with_params(path_utf8.c_str(), cparams);
        if (!impl_->ctx) {
            throw std::runtime_error(
                L("The Whisper model could not be loaded: ",
                  "Whisper modeli yüklenemedi: ") + path_utf8);
        }
        impl_->loaded_path = path_utf8;
    }

    if (audio.empty()) return {};

    whisper_full_params wparams =
        whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);

    wparams.beam_search.beam_size = 5;
    wparams.n_threads             = default_threads(threads_);
    wparams.translate             = false;
    wparams.no_context            = false;  // condition on previous text
    wparams.single_segment        = false;
    wparams.print_special         = false;
    wparams.print_progress        = false;
    wparams.print_realtime        = false;
    wparams.print_timestamps      = false;
    wparams.token_timestamps      = true;   // required for speaker attribution
    wparams.suppress_blank        = true;
    wparams.max_len               = 0;      // let whisper choose segment length
    wparams.entropy_thold         = 2.4f;
    wparams.temperature_inc       = 0.2f;   // fall back on failed decodes

    // "auto" makes whisper detect the language and then transcribe. The
    // detect_language flag is NOT the way to ask for that: whisper_full
    // returns as soon as it has a language, leaving zero segments behind —
    // a silently empty transcript for every recording.
    wparams.language        = language_.empty() ? "auto" : language_.c_str();
    wparams.detect_language = false;

    CallbackState cb_state{&progress, &abort_};
    wparams.progress_callback           = progress_callback;
    wparams.progress_callback_user_data = &cb_state;
    wparams.abort_callback              = abort_callback;
    wparams.abort_callback_user_data    = &cb_state;

    if (progress) progress("", 0.0);

    const int rc = whisper_full(impl_->ctx, wparams,
                                audio.data(), static_cast<int>(audio.size()));
    if (rc != 0) {
        if (abort_.load()) {
            throw std::runtime_error(L("Transcription was cancelled.",
                                       "Metne dönüştürme iptal edildi."));
        }
        throw std::runtime_error(L("Transcription failed (whisper_full=",
                                   "Metne dönüştürme başarısız (whisper_full=") +
                                 std::to_string(rc) + ").");
    }

    std::vector<TranscriptSegment> out;
    const int n_segments = whisper_full_n_segments(impl_->ctx);
    out.reserve(static_cast<std::size_t>(n_segments));

    for (int i = 0; i < n_segments; ++i) {
        TranscriptSegment seg;
        seg.start = cs_to_sec(whisper_full_get_segment_t0(impl_->ctx, i));
        seg.end   = cs_to_sec(whisper_full_get_segment_t1(impl_->ctx, i));

        const char* raw = whisper_full_get_segment_text(impl_->ctx, i);
        seg.text = trim(raw ? raw : "");
        if (seg.text.empty()) continue;

        // Rebuild words from tokens: whisper emits sub-word pieces, and a piece
        // that starts with a space begins a new word.
        const int n_tokens = whisper_full_n_tokens(impl_->ctx, i);
        for (int j = 0; j < n_tokens; ++j) {
            const whisper_token id = whisper_full_get_token_id(impl_->ctx, i, j);
            if (id >= whisper_token_eot(impl_->ctx)) continue;   // special token

            const char* piece = whisper_full_get_token_text(impl_->ctx, i, j);
            if (!piece || !*piece) continue;

            const whisper_token_data td =
                whisper_full_get_token_data(impl_->ctx, i, j);

            const bool starts_word = (piece[0] == ' ') || seg.words.empty();
            if (starts_word) {
                Word w;
                w.text  = piece;
                w.start = cs_to_sec(td.t0);
                w.end   = cs_to_sec(td.t1);
                seg.words.push_back(std::move(w));
            } else {
                Word& w = seg.words.back();
                w.text += piece;
                w.end = cs_to_sec(td.t1);
            }
        }

        // Token timestamps occasionally come back zeroed; fall back to the
        // segment span so attribution still has something to overlap against.
        for (Word& w : seg.words) {
            if (w.end <= w.start) {
                w.start = seg.start;
                w.end   = seg.end;
            }
        }
        out.push_back(std::move(seg));
    }

    if (progress) progress("", 1.0);
    return out;
}

}  // namespace transcriptor::stt
