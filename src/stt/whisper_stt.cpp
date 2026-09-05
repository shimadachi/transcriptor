#include "stt/whisper_stt.h"
#include "util/cpu.h"
#include "util/lang.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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

// -- subtitle-credit hallucinations ---------------------------------------
//
// Whisper was trained largely on video paired with scraped subtitles, and a
// large share of those subtitle files ended with a translator credit laid over
// silence or an end card. That teaches a strong prior: near-silent audio at the
// end of a clip -> emit the credit. Short recordings hit it hardest, because a
// few seconds of speech is zero-padded to the encoder's fixed 30 s window, so
// most of what the decoder sees is exactly the context those credits were
// trained on. Turkish models land on "Altyazı M.K." almost every time.
//
// The decoding-side knobs don't reach this. The phrase is memorised, so it
// decodes with high confidence and sails past both no_speech_thold and
// logprob_thold; suppress_nst only masks symbols, and the credits are plain
// words. Filtering the decoded text is the only thing that reliably catches it.

// Reduces text to a comparison key: Turkish letters mapped to ASCII, case
// dropped, and everything that isn't a letter or digit removed -- so
// "Altyazı M.K.", "ALTYAZI: M.K." and "altyazi mk" all fold to "altyazimk".
std::string fold(const std::string& s) {
    // Keyed by the two UTF-8 bytes of each Turkish letter, both cases.
    static const std::unordered_map<std::uint16_t, char> kTurkish = {
        {0xC3A7, 'c'}, {0xC387, 'c'},   // ç Ç
        {0xC49F, 'g'}, {0xC49E, 'g'},   // ğ Ğ
        {0xC4B1, 'i'}, {0xC4B0, 'i'},   // ı İ
        {0xC3B6, 'o'}, {0xC396, 'o'},   // ö Ö
        {0xC59F, 's'}, {0xC59E, 's'},   // ş Ş
        {0xC3BC, 'u'}, {0xC39C, 'u'},   // ü Ü
    };

    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (std::isalnum(c)) {
                out += static_cast<char>(std::tolower(c));
            }
            ++i;
            continue;
        }
        if (i + 1 < s.size()) {
            const auto pair = static_cast<std::uint16_t>(
                (c << 8) | static_cast<unsigned char>(s[i + 1]));
            const auto it = kTurkish.find(pair);
            if (it != kTurkish.end()) {
                out += it->second;
                i += 2;
                continue;
            }
        }
        // Any other non-ASCII character: drop the whole UTF-8 sequence.
        for (++i; i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80; ++i) {
        }
    }
    return out;
}

// Matched against the *whole* of a segment (or of a trailing run of words), never
// as a substring of running speech -- "Altyazıları açar mısın?" has to survive.
// Extend this list as new ones turn up; keep entries in folded form.
bool is_credit(const std::string& key) {
    static const std::unordered_set<std::string> kCredits = {
        // Turkish
        "altyazimk",
        "altyazimkcom",
        "altyaziauthor",
        "aboneolmayiunutmayin",
        "kanalimaaboneolmayiunutmayin",
        "videoyubegendiyseniz",
        "bubolumunbetimlemesi",
        // English / generic subtitle-site credits
        "subtitlesbytheamaraorgcommunity",
        "subtitlesbyamaraorg",
        "amaraorg",
        "thanksforwatching",
        "thankyouforwatching",
        "pleasesubscribe",
        "subscribetomychannel",
    };
    return !key.empty() && kCredits.count(key) > 0;
}

// Rebuilds text and end time after words have been removed.
void resync_from_words(TranscriptSegment& seg) {
    std::string text;
    for (const Word& w : seg.words) text += w.text;
    seg.text = trim(text);
    if (!seg.words.empty()) seg.end = seg.words.back().end;
}

// Whisper often appends the credit to real speech in one segment, e.g.
// "...görüşmek üzere. Altyazı M.K." -- trim just the trailing words so the
// real text survives. Bounded because a credit is never long.
void trim_trailing_credit(TranscriptSegment& seg) {
    // Needs at least one word left over; a lone credit word is the whole-segment
    // case, already handled before the token loop.
    if (seg.words.size() < 2) return;

    const std::size_t max_tail = std::min<std::size_t>(8, seg.words.size() - 1);
    std::string tail;
    for (std::size_t n = 1; n <= max_tail; ++n) {
        tail = fold(seg.words[seg.words.size() - n].text) + tail;
        if (is_credit(tail)) {
            seg.words.erase(seg.words.end() - static_cast<std::ptrdiff_t>(n),
                            seg.words.end());
            resync_from_words(seg);
            return;
        }
    }
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

        // A segment that is nothing but a credit is pure hallucination.
        if (is_credit(fold(seg.text))) continue;

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

        // ...and a credit tacked onto the end of real speech loses just the tail.
        trim_trailing_credit(seg);
        if (seg.text.empty()) continue;

        out.push_back(std::move(seg));
    }

    if (progress) progress("", 1.0);
    return out;
}

}  // namespace transcriptor::stt
