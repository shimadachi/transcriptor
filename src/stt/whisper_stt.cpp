#include "stt/whisper_stt.h"
#include "stt/credits.h"
#include "util/cpu.h"
#include "util/lang.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
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

// -- hallucination over silence -------------------------------------------
//
// Whisper was trained largely on video paired with scraped subtitles, and a
// large share of those subtitle files ended with a translator credit laid over
// silence or an end card. That teaches a strong prior: near-silent audio ->
// emit the credit. Turkish models land on "Altyazı M.K." or on a broadcaster's
// audio-description disclaimer almost every time.
//
// The decoding-side knobs don't reach this. The phrase is memorised, so it
// decodes with high confidence and sails past both no_speech_thold and
// logprob_thold; suppress_nst only masks symbols, and the credits are plain
// words. Measured: beam search with a higher temperature fallback, a
// no_speech_thold of 0.4 against a logprob_thold of -0.5, and suppress_nst
// each produced the same disclaimer, word for word.
//
// Two things answer it. The voice-activity detector below keeps the silence
// away from the decoder in the first place, which is the actual fix; the text
// filter in credits.cpp is what catches whatever still gets through.

// The detector costs about 2 seconds of CPU per 7 minutes of audio, and it runs
// as one pass before the decode, where whisper's abort callback cannot reach it.
// So Cancel waits it out: half a minute on a recording long enough to notice,
// in front of a decode that will take very much longer than that.
//
// The defaults (30 ms of padding, 100 ms of silence to end a segment) hand
// whisper speech spliced so tightly that it loses sentence boundaries: the
// transcript comes back as long lowercase runs with no punctuation, because
// every pause the decoder segments on has been cut out. Keeping a third of a
// second either side of each speech region, and only treating silence as a
// break after 700 ms, gives it back.
whisper_vad_params vad_params() {
    whisper_vad_params p = whisper_vad_default_params();
    p.speech_pad_ms           = 400;
    p.min_silence_duration_ms = 700;
    // Overlap exists to stop a word being cut in half at a segment join, which
    // the padding above already covers; left in, it repeats the overlapped
    // words in the transcript.
    p.samples_overlap         = 0.0f;
    return p;
}

int default_threads(int configured) {
    if (configured > 0) return configured;
    // Physical cores, not logical: SMT siblings share one core's vector units,
    // so a second thread on the same core contends rather than adds. Counting
    // logical processors put 8 threads on a 4-core laptop, which is slower than
    // 4 -- measured on the diarizer, whose ONNX session has the same shape.
    // The ceiling stays: whisper scales poorly past ~8 either way.
    return static_cast<int>(std::clamp(util::physical_cores(), 1u, 8u));
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

// The decoded segments, words rebuilt from the tokens and credits taken out.
// Shared by both transcribers, so the preview and the finished transcript are
// filtered the same way. `silence_removed`: the voice detector ran in front of
// this decode -- see strip_credits().
std::vector<TranscriptSegment> read_segments(whisper_context* ctx, bool silence_removed) {
    std::vector<TranscriptSegment> out;
    const int n_segments = whisper_full_n_segments(ctx);
    out.reserve(static_cast<std::size_t>(n_segments));

    for (int i = 0; i < n_segments; ++i) {
        TranscriptSegment seg;
        seg.start = cs_to_sec(whisper_full_get_segment_t0(ctx, i));
        seg.end   = cs_to_sec(whisper_full_get_segment_t1(ctx, i));

        const char* raw = whisper_full_get_segment_text(ctx, i);
        seg.text = trim(raw ? raw : "");
        if (seg.text.empty()) continue;

        // Rebuild words from tokens: whisper emits sub-word pieces, and a piece
        // that starts with a space begins a new word.
        const int n_tokens = whisper_full_n_tokens(ctx, i);
        for (int j = 0; j < n_tokens; ++j) {
            const whisper_token id = whisper_full_get_token_id(ctx, i, j);
            if (id >= whisper_token_eot(ctx)) continue;   // special token

            const char* piece = whisper_full_get_token_text(ctx, i, j);
            if (!piece || !*piece) continue;

            // These accessors, not whisper_full_get_token_data(): with VAD on,
            // the decoder works on audio with the silence cut out, and the
            // token struct still holds timestamps in that compressed time.
            // Only the accessors map them back to where the words really are --
            // which is what speaker attribution and the [mm:ss] marks need.
            const double t0 = cs_to_sec(whisper_full_get_token_t0(ctx, i, j));
            const double t1 = cs_to_sec(whisper_full_get_token_t1(ctx, i, j));

            const bool starts_word = (piece[0] == ' ') || seg.words.empty();
            if (starts_word) {
                Word w;
                w.text  = piece;
                w.start = t0;
                w.end   = t1;
                seg.words.push_back(std::move(w));
            } else {
                Word& w = seg.words.back();
                w.text += piece;
                w.end = t1;
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

        // A segment that is nothing but a hallucinated credit goes entirely;
        // one tacked onto the end of real speech loses just the tail.
        if (!strip_credits(seg, silence_removed)) continue;

        out.push_back(std::move(seg));
    }

    return out;
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
    const paths::fs::path& vad_model_path, const ProgressFn& progress) {

    // The abort flag is cleared when a job is admitted, not here -- see
    // reset_abort(). Clearing it on entry lost every cancellation raised
    // between admission and this line.
    if (abort_.load()) {
        throw std::runtime_error(L("Transcription was cancelled.",
                                   "Metne dönüştürme iptal edildi."));
    }

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
        // Counted over GPU *and* integrated GPU devices, in registry order --
        // the same walk whisper does internally. Left at the default 0, a
        // laptop with an iGPU first in the list ran everything on the iGPU no
        // matter which card the badge named.
        cparams.gpu_device = device_.gpu_index;

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
    // Conditioning each window on the text decoded so far reads well until the
    // model repeats itself once: the repeat is then in the prompt, which makes
    // the next window likelier to repeat it, and the loop has no way out. A
    // 1h53m recording came back as one sentence 1717 times, from 02:47 to the
    // end of the file.
    //
    // Both switches are needed. no_context only clears the carried prompt once,
    // as whisper_full starts, which separates one run from the next; the rolling
    // context *within* a run is rebuilt from each window's own output and gated
    // on n_max_text_ctx instead (whisper.cpp's -mc). Zero is what turns it off.
    wparams.no_context            = true;
    wparams.n_max_text_ctx        = 0;
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

    // Silence never reaches the decoder: whisper runs the Silero detector first
    // and transcribes only the speech it finds, mapping the timestamps back to
    // real time afterwards. This is what stops the hallucinated credits, and it
    // stops them costing real words: a credit decoded over a silent opening
    // does not stay in the silence, it fills the whole first 30 s window, and
    // the speech inside that window goes down with it.
    //
    // Optional by design. The model is under a megabyte and is fetched with the
    // speech weights, but an install that has not got it yet, or could not
    // reach the network, transcribes the old way rather than failing.
    const std::string vad_utf8 = paths::to_utf8(vad_model_path);
    const bool use_vad = !vad_utf8.empty();
    if (use_vad) {
        wparams.vad            = true;
        wparams.vad_model_path = vad_utf8.c_str();
        wparams.vad_params     = vad_params();
    }

    CallbackState cb_state{&progress, &abort_};
    wparams.progress_callback           = progress_callback;
    wparams.progress_callback_user_data = &cb_state;
    wparams.abort_callback              = abort_callback;
    wparams.abort_callback_user_data    = &cb_state;

    if (progress) progress("", 0.0);

    int rc = whisper_full(impl_->ctx, wparams,
                          audio.data(), static_cast<int>(audio.size()));

    // -1 is whisper_full's own code for "the VAD step failed", and the only
    // thing that returns it; everything the decoder itself can fail at is -2 or
    // lower. A corrupt or half-downloaded detector model must not cost the user
    // the transcription, so the run is repeated without it. It costs nothing in
    // practice: VAD runs before the decode, so this fails in the first second.
    if (rc == -1 && use_vad && !abort_.load()) {
        std::fprintf(stderr,
                     "whisper: the voice detector could not be used (%s); "
                     "transcribing the whole recording instead\n",
                     vad_utf8.c_str());
        wparams.vad = false;
        rc = whisper_full(impl_->ctx, wparams,
                          audio.data(), static_cast<int>(audio.size()));
    }

    if (rc != 0) {
        if (abort_.load()) {
            throw std::runtime_error(L("Transcription was cancelled.",
                                       "Metne dönüştürme iptal edildi."));
        }
        throw std::runtime_error(L("Transcription failed (whisper_full=",
                                   "Metne dönüştürme başarısız (whisper_full=") +
                                 std::to_string(rc) + ").");
    }

    // wparams.vad, not use_vad: a detector that failed was retried without.
    std::vector<TranscriptSegment> out = read_segments(impl_->ctx, wparams.vad);

    if (progress) progress("", 1.0);
    return out;
}

// ---------------------------------------------------------------------------
// Live
// ---------------------------------------------------------------------------

namespace {

bool live_abort_callback(void* user_data) {
    const auto* abort = static_cast<const std::atomic<bool>*>(user_data);
    return abort && abort->load();
}

// Whisper reports every decode on stderr, a dozen lines at a time. Once per
// transcription that is a useful record; the live transcript decodes every
// second, which made it tens of thousands of lines an hour. Whisper has one
// logger for the whole process, so it is swapped once for one that drops the
// chatter from a live decode's own thread and passes everything else through
// as whisper's default does -- the offline run logs exactly as it always has,
// and a warning or an error from either still gets out.
thread_local bool t_live_decode = false;
thread_local bool t_dropping    = false;   // what to do with a continuation line

void live_aware_log(ggml_log_level level, const char* text, void*) {
    if (level != GGML_LOG_LEVEL_CONT) {
        t_dropping = level == GGML_LOG_LEVEL_DEBUG ||
                     (t_live_decode && level < GGML_LOG_LEVEL_WARN);
    }
    if (!t_dropping && text) std::fputs(text, stderr);
}

void install_logger_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        // whisper_log_set() also takes over ggml's logger, which is not
        // whisper's to change: the summarizer quiets it. Put it back as it was.
        ggml_log_callback ggml_cb = nullptr;
        void*             ggml_data = nullptr;
        ggml_log_get(&ggml_cb, &ggml_data);
        whisper_log_set(live_aware_log, nullptr);
        ggml_log_set(ggml_cb, ggml_data);
    });
}

struct LiveDecodeScope {
    LiveDecodeScope()  { t_live_decode = true; }
    ~LiveDecodeScope() { t_live_decode = false; }
};

}  // namespace

struct LiveWhisper::Impl {
    whisper_context* ctx = nullptr;
    std::string      loaded_path;
    // Which device the context was made for: the same file on another card is
    // a different load.
    bool             loaded_gpu   = false;
    int              loaded_index = 0;
    // The language the first window with words in it was detected as, held
    // for the rest of the take. Empty until then, and whenever a language was
    // asked for rather than detected.
    std::string      detected;
    // The voice detector failed to load this take. See transcribe().
    bool             vad_failed = false;

    ~Impl() {
        if (ctx) whisper_free(ctx);
    }
};

LiveWhisper::LiveWhisper() : impl_(std::make_unique<Impl>()) { install_logger_once(); }

LiveWhisper::~LiveWhisper() = default;

bool LiveWhisper::loaded() const { return impl_->ctx != nullptr; }

void LiveWhisper::unload() {
    if (impl_->ctx) {
        whisper_free(impl_->ctx);
        impl_->ctx = nullptr;
        impl_->loaded_path.clear();
    }
}

void LiveWhisper::begin_take() {
    impl_->detected.clear();
    impl_->vad_failed = false;
}

void LiveWhisper::load(const paths::fs::path& model_path, const DeviceInfo& device) {
    const std::string path_utf8 = paths::to_utf8(model_path);
    if (impl_->ctx && (impl_->loaded_path != path_utf8 ||
                       impl_->loaded_gpu != device.use_gpu() ||
                       impl_->loaded_index != device.gpu_index)) {
        unload();
    }
    if (impl_->ctx) return;

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu    = device.use_gpu();
    cparams.gpu_device = device.gpu_index;   // see WhisperTranscriber::transcribe

    impl_->ctx = whisper_init_from_file_with_params(path_utf8.c_str(), cparams);
    if (!impl_->ctx) {
        throw std::runtime_error(L("The live transcript model could not be loaded: ",
                                   "Canlı metin modeli yüklenemedi: ") + path_utf8);
    }
    impl_->loaded_path  = path_utf8;
    impl_->loaded_gpu   = device.use_gpu();
    impl_->loaded_index = device.gpu_index;
}

std::vector<TranscriptSegment> LiveWhisper::transcribe(
    const std::vector<float>& audio, const std::string& language,
    const paths::fs::path& vad_model_path, int threads,
    const std::atomic<bool>* abort) {

    if (!impl_->ctx) {
        throw std::runtime_error(L("The live transcript model is not loaded.",
                                   "Canlı metin modeli yüklü değil."));
    }
    if (audio.empty() || (abort && abort->load())) return {};
    const LiveDecodeScope quiet;

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.n_threads        = default_threads(threads);
    wparams.translate        = false;
    // The same two switches as the offline run, for the same reason: no window
    // is decoded with another window's words as its prompt.
    wparams.no_context       = true;
    wparams.n_max_text_ctx   = 0;
    wparams.single_segment   = false;
    wparams.print_special    = false;
    wparams.print_progress   = false;
    wparams.print_realtime   = false;
    wparams.print_timestamps = false;
    // Nothing downstream attributes a preview line to a speaker.
    wparams.token_timestamps = false;
    wparams.suppress_blank   = true;
    wparams.temperature_inc  = 0.0f;   // see the class comment

    const std::string& lang = !language.empty() ? language : impl_->detected;
    wparams.language        = lang.empty() ? "auto" : lang.c_str();
    wparams.detect_language = false;

    // A preview that hallucinates subtitle credits every time the room goes
    // quiet would be worse than none, and silence is most of what a live
    // window holds. Same detector, same settings as the offline run.
    const std::string vad_utf8 = paths::to_utf8(vad_model_path);
    const bool use_vad = !vad_utf8.empty() && !impl_->vad_failed;
    if (use_vad) {
        wparams.vad            = true;
        wparams.vad_model_path = vad_utf8.c_str();
        wparams.vad_params     = vad_params();
    }

    wparams.abort_callback           = live_abort_callback;
    wparams.abort_callback_user_data = const_cast<std::atomic<bool>*>(abort);

    int rc = whisper_full(impl_->ctx, wparams, audio.data(), static_cast<int>(audio.size()));
    if (rc == -1 && use_vad && !(abort && abort->load())) {
        // The detector failed; see the offline run. Once is enough to know, so
        // the rest of the take goes without it rather than paying for the same
        // failure every second.
        std::fprintf(stderr,
                     "whisper: the voice detector could not be used (%s); "
                     "the live transcript carries on without it\n",
                     vad_utf8.c_str());
        impl_->vad_failed = true;
        wparams.vad = false;
        rc = whisper_full(impl_->ctx, wparams, audio.data(), static_cast<int>(audio.size()));
    }
    if (abort && abort->load()) return {};
    if (rc != 0) {
        throw std::runtime_error(L("The live transcript failed (whisper_full=",
                                   "Canlı metin başarısız oldu (whisper_full=") +
                                 std::to_string(rc) + ").");
    }

    std::vector<TranscriptSegment> out = read_segments(impl_->ctx, wparams.vad);
    if (language.empty() && impl_->detected.empty() && !out.empty()) {
        const char* code = whisper_lang_str(whisper_full_lang_id(impl_->ctx));
        if (code) impl_->detected = code;
    }
    return out;
}

}  // namespace transcriptor::stt
