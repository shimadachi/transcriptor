#include "pipeline/live.h"

#include <algorithm>
#include <cmath>
#include <exception>

#include "pipeline/processor.h"
#include "stt/credits.h"
#include "util/lang.h"

namespace transcriptor::pipeline {

namespace {

// A window this short holds nothing a model can make a word of. The tail pass
// stops here rather than decode a fragment into a hallucination.
constexpr double kShortest = 0.3;

std::size_t samples(double seconds, int samplerate) {
    return static_cast<std::size_t>(std::max(0.0, seconds) * samplerate);
}

double round2(double v) { return std::round(v * 100.0) / 100.0; }

std::string join_text(const std::vector<stt::TranscriptSegment>& segs,
                      std::size_t from) {
    std::string out;
    for (std::size_t i = from; i < segs.size(); ++i) {
        if (!out.empty()) out += ' ';
        out += segs[i].text;
    }
    return out;
}

}  // namespace

LiveTranscriber::LiveTranscriber(LiveTuning tuning) : tuning_(tuning) {}

LiveTranscriber::~LiveTranscriber() {
    stop();
    join();
}

void LiveTranscriber::begin(LiveEngine engine, int samplerate) {
    std::lock_guard<std::mutex> tl(thread_mutex_);
    // Whatever ran before is ended, not finished: it was told to finish when
    // its take or its switch went, and a new session now is the user asking
    // for the present. What it was still hearing is kept as it stood.
    abort_.store(true);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    abort_.store(false);

    std::uint64_t session = 0;
    {
        std::lock_guard<std::mutex> lock(m_);
        session     = ++session_;
        samplerate_ = samplerate > 0 ? samplerate : 16000;
        took_part_  = true;
        active_     = true;
        accepting_  = true;
        finishing_  = false;
        state_      = "loading";
        error_.clear();
        buf_.clear();
        have_start_ = false;
        decoded_    = 0;
        hyp_.clear();
        partial_.clear();
        ++rev_;
    }
    thread_ = std::thread(&LiveTranscriber::run, this, session, std::move(engine));
}

void LiveTranscriber::feed(const std::vector<float>& block, std::size_t offset) {
    std::lock_guard<std::mutex> lock(m_);
    if (!accepting_ || block.empty()) return;
    // What of the take this has heard, for kept_transcript(). A block that does
    // not start where the last one ended means Live was off in between.
    if (!heard_) {
        heard_      = true;
        heard_from_ = offset;
    } else if (offset != heard_to_) {
        heard_gap_ = true;
    }
    heard_to_ = offset + block.size();
    if (!have_start_) {
        buf_start_  = offset;
        have_start_ = true;
    }
    buf_.insert(buf_.end(), block.begin(), block.end());
    if (buf_.size() >= decoded_ + samples(tuning_.step, samplerate_)) cv_.notify_all();
}

void LiveTranscriber::finish() {
    std::lock_guard<std::mutex> lock(m_);
    if (!active_ || finishing_) return;
    accepting_ = false;
    finishing_ = true;
    if (state_ != "loading") state_ = "finishing";
    ++rev_;
    cv_.notify_all();
}

void LiveTranscriber::stop() {
    abort_.store(true);
    std::lock_guard<std::mutex> lock(m_);
    accepting_ = false;
    cv_.notify_all();
}

void LiveTranscriber::join() {
    std::lock_guard<std::mutex> tl(thread_mutex_);
    if (thread_.joinable()) thread_.join();
}

void LiveTranscriber::clear() {
    abort_.store(true);
    std::lock_guard<std::mutex> lock(m_);
    // A new number for both: the session still unwinding can no longer write
    // here, and a page holding the old text knows to drop it.
    ++session_;
    ++take_;
    ++rev_;
    took_part_  = false;
    heard_      = false;
    heard_from_ = 0;
    heard_to_   = 0;
    heard_gap_  = false;
    cut_short_  = false;
    active_    = false;
    accepting_ = false;
    finishing_ = false;
    lagging_   = false;
    state_     = "idle";
    error_.clear();
    buf_.clear();
    have_start_ = false;
    decoded_    = 0;
    lines_.clear();
    partial_.clear();
    hyp_.clear();
    cv_.notify_all();
}

void LiveTranscriber::run(std::uint64_t session, LiveEngine engine) {
    try {
        engine.prepare();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(m_);
        if (session_ == session) error_ = e.what();
        end_locked(session, false);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_);
        if (session_ == session && state_ == "loading") {
            state_ = finishing_ ? "finishing" : "listening";
            ++rev_;
        }
    }

    const std::size_t cap = samples(tuning_.max_window, samplerate_);
    try {
        for (;;) {
            std::vector<float> window;
            bool last = false;
            {
                std::unique_lock<std::mutex> lock(m_);
                const std::size_t step = samples(tuning_.step, samplerate_);
                cv_.wait(lock, [&] {
                    return abort_.load() || session_ != session || finishing_ ||
                           buf_.size() >= decoded_ + step;
                });
                if (abort_.load() || session_ != session) break;
                last = finishing_;
                // The tail pass stops at a fragment; a live pass decodes what
                // it has, however short, since more is on its way.
                if (last && buf_.size() < samples(kShortest, samplerate_)) {
                    cut_locked(buf_.size());
                    break;
                }
                window.assign(buf_.begin(),
                              buf_.begin() + static_cast<long>(std::min(buf_.size(), cap)));
                decoded_ = window.size();
            }

            std::vector<stt::TranscriptSegment> segs = engine.decode(window, &abort_);
            if (abort_.load()) break;

            std::lock_guard<std::mutex> lock(m_);
            if (session_ != session) break;
            // The tail pass settles everything it hears and goes round again
            // until the buffer is used up.
            apply_locked(std::move(segs), window.size(), last);
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(m_);
        if (session_ == session) error_ = e.what();
    }

    std::lock_guard<std::mutex> lock(m_);
    end_locked(session, true);
}

void LiveTranscriber::apply_locked(std::vector<stt::TranscriptSegment> segs,
                                   std::size_t n, bool all) {
    const double win  = static_cast<double>(n) / samplerate_;
    const double base = static_cast<double>(buf_start_) / samplerate_;

    // Into the window, and in order: a segment's end can come back past the
    // audio it was decoded from.
    for (stt::TranscriptSegment& s : segs) {
        s.start = std::clamp(s.start, 0.0, win);
        s.end   = std::clamp(s.end, s.start, win);
    }

    const std::string before = partial_;
    std::size_t settle = 0;
    std::size_t cut    = 0;

    if (segs.empty()) {
        // Nothing said, or nothing yet. Let the silence go once there is
        // enough of it to be sure, and keep the edge, where a word may be
        // starting that the next decode will want whole.
        if (all) {
            cut = n;
        } else if (win >= tuning_.silence) {
            const std::size_t keep = samples(tuning_.keep_tail, samplerate_);
            cut = n > keep ? n - keep : 0;
        }
    } else {
        // Settled is what this decode and the last one agree on, from the
        // front, short of the last segment -- which is still being spoken
        // and has every right to change. Compared folded, so a comma that
        // comes and goes does not count as disagreement.
        while (settle + 1 < segs.size() && settle < hyp_.size() &&
               stt::fold(segs[settle].text) == hyp_[settle]) {
            ++settle;
        }
        // The last one settles too once the speaker has stopped: silence long
        // enough after it that it cannot still be going. Waiting for the
        // window to fill instead would leave the last sentence before every
        // pause unsettled for twenty seconds.
        const bool over = win - segs.back().end >= tuning_.silence;
        // A full window cuts at a segment boundary where it has one. Only a
        // single segment filling the whole of it is cut at the window's edge,
        // which may fall mid-word -- the price of never letting a window grow
        // past what whisper can take in.
        const bool full = win >= tuning_.max_window;
        if (all || over || (full && segs.size() == 1)) {
            settle = segs.size();
        } else if (full || win >= tuning_.force_commit) {
            settle = segs.size() - 1;
        }

        if (settle == segs.size()) {
            // A window ended by silence keeps its edge, as above -- but never
            // the words just settled, which would be heard twice.
            const std::size_t keep = samples(tuning_.keep_tail, samplerate_);
            const std::size_t said = samples(segs.back().end, samplerate_);
            cut = (all || !over || n <= keep) ? n : std::max(said, n - keep);
        } else if (settle > 0) {
            // Where the next decode starts: the end of the last settled
            // segment, unless the next one began before it did.
            cut = samples(std::min(segs[settle - 1].end, segs[settle].start),
                          samplerate_);
        }
        // Settling without moving on would hand the same words to the next
        // decode, which would say them again.
        if (cut == 0) settle = 0;
    }

    for (std::size_t i = 0; i < settle; ++i) {
        lines_.push_back({base + segs[i].start, base + segs[i].end, segs[i].text});
    }

    hyp_.clear();
    for (std::size_t i = settle; i < segs.size(); ++i) {
        hyp_.push_back(stt::fold(segs[i].text));
    }
    partial_ = join_text(segs, settle);
    if (!partial_.empty()) {
        partial_start_ = base + segs[settle].start;
        partial_end_   = base + segs.back().end;
    }

    cut_locked(std::min(cut, buf_.size()));

    // Further behind than the model can ever make up: let the oldest go and
    // pick up at the present. Marked in the text, so the jump is not mistaken
    // for the room having gone quiet.
    const std::size_t backlog = samples(tuning_.max_backlog, samplerate_);
    const bool skip = buf_.size() > backlog;
    if (skip) {
        // What is kept is the start of the next window -- but never more than
        // the backlog allows, which is less than is buffered by definition of
        // a skip. Keeping force_commit's worth outright underflowed the cut
        // below whenever the backlog was the shorter of the two: it erased far
        // past the end of the buffer, which crashed outright on macOS and
        // Windows and quietly wrecked the buffer on Linux.
        const std::size_t keep =
            std::min(samples(tuning_.force_commit, samplerate_), backlog);
        const double at = static_cast<double>(buf_start_) / samplerate_;
        if (!partial_.empty()) {
            lines_.push_back({partial_start_, partial_end_, partial_});
        }
        lines_.push_back({at, at, "…"});
        partial_.clear();
        hyp_.clear();
        cut_locked(buf_.size() - keep);
        lagging_ = true;
    }

    if (settle > 0 || partial_ != before || skip) ++rev_;
}

void LiveTranscriber::cut_locked(std::size_t n) {
    if (n == 0) return;
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(n));
    buf_start_ += n;
    decoded_ = decoded_ > n ? decoded_ - n : 0;
}

void LiveTranscriber::end_locked(std::uint64_t session, bool keep_partial) {
    // A session that has been replaced or cleared leaves everything to the
    // one that replaced it.
    if (session_ != session) return;
    // Audio still waiting here was never transcribed: the session was stopped
    // rather than finished, or failed. Whatever it held is missing from the
    // text, so the text is not the whole take.
    if (buf_.size() >= samples(kShortest, samplerate_) || !error_.empty()) {
        cut_short_ = true;
    }
    if (keep_partial && !partial_.empty()) {
        lines_.push_back({partial_start_, partial_end_, partial_});
    }
    partial_.clear();
    hyp_.clear();
    buf_.clear();
    decoded_   = 0;
    active_    = false;
    accepting_ = false;
    finishing_ = false;
    state_     = "idle";
    ++rev_;
}

nlohmann::json LiveTranscriber::status_json() const {
    std::lock_guard<std::mutex> lock(m_);
    return {
        {"take", take_},
        {"rev", rev_},
        {"active", active_},
        {"state", state_},
        {"count", lines_.size()},
        {"has_text", !lines_.empty() || !partial_.empty()},
        {"lagging", lagging_},
        {"error", error_.empty() ? nlohmann::json(nullptr) : nlohmann::json(error_)},
    };
}

bool LiveTranscriber::heard_take() const {
    std::lock_guard<std::mutex> lock(m_);
    return took_part_;
}

std::optional<ProcessResult> LiveTranscriber::kept_transcript(
    std::size_t take_samples, std::string* why) const {
    std::lock_guard<std::mutex> lock(m_);
    const auto missed = [why](const std::string& reason) {
        if (why) *why = reason;
        return std::optional<ProcessResult>();
    };
    if (why) why->clear();
    if (!took_part_) return std::nullopt;   // Live was off: nothing to explain

    // In the order a person would look for the cause.
    if (active_) {
        return missed(L("it had not finished.", "bitmemişti."));
    }
    if (!error_.empty()) {
        // The error is someone else's sentence; close it so the advice the
        // caller adds after it does not run on.
        const char last = error_.back();
        const std::string end = (last == '.' || last == '!' || last == '?') ? "" : ".";
        return missed(L("it stopped on an error — ", "bir hatayla durdu — ") + error_ + end);
    }
    if (!heard_) {
        return missed(L("Live heard none of the take.",
                        "Canlı metin kaydın hiçbir bölümünü duymadı."));
    }
    if (heard_from_ > 0) {
        return missed(L("Live came on ", "Canlı metin kaydın ") +
                      fmt_ts(static_cast<double>(heard_from_) / samplerate_) +
                      L(" into the take, so it missed the start.",
                        " noktasında açıldı, başını kaçırdı."));
    }
    if (heard_gap_) {
        return missed(L("Live was off for part of the take.",
                        "Canlı metin kaydın bir bölümünde kapalıydı."));
    }
    if (heard_to_ < take_samples) {
        return missed(L("Live was switched off before the take ended.",
                        "Canlı metin kayıt bitmeden kapatıldı."));
    }
    if (lagging_) {
        return missed(L("the live model fell behind and skipped part of the take.",
                        "canlı model geride kaldı ve kaydın bir bölümünü atladı."));
    }
    if (cut_short_) {
        return missed(L("it was stopped before it finished.", "bitmeden durduruldu."));
    }

    ProcessResult result;
    result.duration = static_cast<double>(take_samples) / samplerate_;
    result.lines.reserve(lines_.size());
    for (const Line& l : lines_) result.lines.push_back({-1, l.text, l.start, l.end});
    return result;
}

nlohmann::json LiveTranscriber::text_json(std::size_t from) const {
    std::lock_guard<std::mutex> lock(m_);
    if (from > lines_.size()) from = 0;
    nlohmann::json lines = nlohmann::json::array();
    for (std::size_t i = from; i < lines_.size(); ++i) {
        const Line& l = lines_[i];
        lines.push_back({{"start", round2(l.start)},
                         {"end", round2(l.end)},
                         {"ts", fmt_ts(l.start)},
                         {"text", l.text}});
    }
    return {
        {"take", take_},
        {"rev", rev_},
        {"from", from},
        {"count", lines_.size()},
        {"lines", lines},
        {"partial", partial_.empty() ? nlohmann::json(nullptr)
                                     : nlohmann::json(partial_)},
        {"partial_ts", partial_.empty() ? nlohmann::json(nullptr)
                                        : nlohmann::json(fmt_ts(partial_start_))},
    };
}

}  // namespace transcriptor::pipeline
