// The live transcript: text on screen while a take is still being recorded.
//
// The recorder hands every block it keeps to feed(). A thread of its own
// decodes the stretch of audio that has not settled yet, again each time it
// has grown by another second, and moves a line into the transcript once two
// decodes in a row agree on it -- or once that stretch is long enough that
// waiting for agreement would only leave the preview further behind. What has
// not settled is shown as well, as the line still being spoken.
//
// It is a preview. Nothing here is saved, and the transcript made once the take
// ends replaces it on screen; that one is made the careful way, with the
// offline model, and this one is made to keep up.
//
// The speech model is not in here: begin() is handed one. That keeps the file
// free of whisper, so the windowing can be driven by a stand-in.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "stt/whisper_stt.h"

namespace transcriptor::pipeline {

struct LiveEngine {
    // Runs once, on the session's own thread, before anything is decoded: the
    // model load, which can take seconds and must not hold up the recording.
    // Throws std::runtime_error with a user-facing message.
    std::function<void()> prepare;

    // Segment times are relative to the first sample of `audio`. Returns
    // early, with nothing, once *abort is raised. Throws std::runtime_error
    // with a user-facing message when the decode fails.
    std::function<std::vector<stt::TranscriptSegment>(
        const std::vector<float>& audio, const std::atomic<bool>* abort)> decode;
};

// How the stream is cut up, in seconds of audio. The defaults are what the app
// runs with.
struct LiveTuning {
    // New audio needed before the window is decoded again.
    double step = 1.0;
    // A window this long settles all but its last segment, agreed or not: a
    // speaker who never pauses would otherwise never see a line settle.
    double force_commit = 10.0;
    // A window this long settles everything in it, so no window ever reaches
    // the 30 s whisper can take in at once.
    double max_window = 20.0;
    // Audio waiting behind the window, beyond which the model is not keeping
    // up and the preview skips ahead to the present rather than fall further
    // and further behind the room.
    double max_backlog = 40.0;
    // A window with no speech in it is let go once it is this long...
    double silence = 3.0;
    // ...except for its last stretch, in case a word is starting at its edge.
    double keep_tail = 1.0;
};

class LiveTranscriber {
public:
    explicit LiveTranscriber(LiveTuning tuning = {});
    ~LiveTranscriber();

    LiveTranscriber(const LiveTranscriber&) = delete;
    LiveTranscriber& operator=(const LiveTranscriber&) = delete;

    // Starts listening, ending any session still running first -- and waiting
    // for it, so two sessions never share the engine. The text already there
    // stays: Live switched off and on again mid-take carries on the same
    // transcript. clear() is what starts a fresh one.
    void begin(LiveEngine engine, int samplerate);

    // Audio for the running session; ignored when none is listening. `offset`
    // is where the block starts in the take, in samples, so every line is
    // stamped with its place in the recording however late Live came in.
    void feed(const std::vector<float>& block, std::size_t offset);

    // The take ended, or Live was switched off: transcribe what is left, keep
    // it, and end. Returns at once; the session finishes on its own thread.
    void finish();

    // End now. What was still being spoken is kept as it last stood. Returns
    // at once; join() waits.
    void stop();

    // Waits for the session thread, if there is one.
    void join();

    // Drops the text along with the session: a new take, or a cancelled one.
    void clear();

    // For /api/state: whether a session runs, how far the text has got, and
    // what went wrong if anything did.
    nlohmann::json status_json() const;

    // For /api/live: the lines from index `from` on, and the line still being
    // spoken. A `from` past the end -- a page that missed a clear() -- is
    // answered from the start, and the reply's own "from" says which it got.
    nlohmann::json text_json(std::size_t from) const;

private:
    struct Line {
        double      start = 0.0;   // seconds into the take
        double      end   = 0.0;
        std::string text;
    };

    void run(std::uint64_t session, LiveEngine engine);

    // Folds one decode of the first `n` samples of buf_ into the text. With
    // `all`, everything in it settles. Caller holds m_.
    void apply_locked(std::vector<stt::TranscriptSegment> segs, std::size_t n,
                      bool all);
    void cut_locked(std::size_t n);
    void end_locked(std::uint64_t session, bool keep_partial);

    const LiveTuning tuning_;

    // Guards thread_ alone, so begin() and join() from different threads can
    // never join the same thread twice. Taken before m_, never inside it; the
    // session thread never takes it at all.
    std::mutex  thread_mutex_;
    std::thread thread_;

    // Raised to make the running session give up, decode and all.
    std::atomic<bool> abort_{false};

    mutable std::mutex      m_;
    std::condition_variable cv_;

    // Everything below is guarded by m_.

    // Which session may write here. A session that has been replaced or
    // cleared finds its number gone and keeps its results to itself.
    std::uint64_t session_ = 0;
    // Which transcript the text belongs to: bumped by clear() alone, so a page
    // can tell "start again" from "here is more".
    std::uint64_t take_ = 0;
    // Bumped on every change a page would want to redraw for.
    std::uint64_t rev_ = 0;

    int  samplerate_ = 16000;
    bool active_     = false;   // a session is running
    bool accepting_  = false;   // feed() keeps what it is given
    bool finishing_  = false;
    bool lagging_    = false;   // the session has had to skip ahead
    std::string state_ = "idle";   // idle | loading | listening | finishing
    std::string error_;

    // Audio that has not settled yet, and where it starts in the take.
    std::vector<float> buf_;
    std::size_t        buf_start_ = 0;
    bool               have_start_ = false;
    // How much of buf_ the last decode covered.
    std::size_t        decoded_ = 0;

    std::vector<Line>        lines_;
    std::string              partial_;
    double                   partial_start_ = 0.0;
    double                   partial_end_   = 0.0;
    // The unsettled segments of the last decode, folded, for the agreement
    // test against the next one.
    std::vector<std::string> hyp_;
};

}  // namespace transcriptor::pipeline
