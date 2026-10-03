// Regression tests for keeping the live transcript (V55).
//
// V55: the live transcript was thrown away at Stop. A take recorded with Live
// on had its text on screen the whole time, and still had no transcript until
// the offline model had been through the same audio again. It is kept now --
// but only when it heard the whole take, because one that missed a stretch,
// saved as though it had not, loses that stretch from the transcript with
// nothing to say so.
//
// The real LiveTranscriber, driven by a stand-in engine: every decode comes
// back as one segment naming how much audio it was given, so a test can see
// what was heard without a model.

#include "pipeline/live.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "check.h"

using namespace transcriptor;
using pipeline::LiveEngine;
using pipeline::LiveTranscriber;

namespace {

constexpr int kRate = 16000;

LiveEngine engine(int decode_ms = 0, bool fail_to_load = false) {
    LiveEngine e;
    e.prepare = [fail_to_load] {
        if (fail_to_load) throw std::runtime_error("the model would not load");
    };
    e.decode = [decode_ms](const std::vector<float>& audio, const std::atomic<bool>* abort) {
        if (decode_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(decode_ms));
        std::vector<stt::TranscriptSegment> out;
        if (abort && abort->load()) return out;
        stt::TranscriptSegment s;
        s.start = 0.0;
        s.end   = static_cast<double>(audio.size()) / kRate;
        s.text  = "heard " + std::to_string(audio.size());
        out.push_back(s);
        return out;
    };
    return e;
}

// Hands the transcriber `seconds` of take, in quarter-second blocks the way the
// recorder does, starting `from` samples into it. Returns where it stopped.
std::size_t feed(LiveTranscriber& live, std::size_t from, double seconds) {
    const std::size_t block = kRate / 4;
    const std::size_t end = from + static_cast<std::size_t>(seconds * kRate);
    for (std::size_t at = from; at < end; at += block) {
        live.feed(std::vector<float>(std::min(block, end - at), 0.1f), at);
    }
    return end;
}

void finish(LiveTranscriber& live) {
    live.finish();
    live.join();
}

std::string missing(const LiveTranscriber& live, std::size_t take, bool* kept) {
    std::string why;
    *kept = live.kept_transcript(take, &why).has_value();
    return why;
}

void a_take_heard_whole_is_kept() {
    LiveTranscriber live;
    live.begin(engine(), kRate);
    const std::size_t take = feed(live, 0, 5.0);
    finish(live);

    std::string why;
    const auto kept = live.kept_transcript(take, &why);
    test::check("V55 a take Live heard from start to end is kept", kept.has_value(), why);
    if (!kept) return;
    test::check("V55 with what it heard in it", kept->has_text() && !kept->lines.empty());
    test::check("V55 and nothing claimed about speakers",
                !kept->diarized && kept->lines.front().speaker == -1);
    test::check("V55 its length is the take's",
                kept->duration == static_cast<double>(take) / kRate,
                std::to_string(kept->duration));
}

void a_take_live_missed_the_start_of_is_not() {
    LiveTranscriber live;
    live.begin(engine(), kRate);
    // Switched on three seconds in: the first block it sees is not the first.
    const std::size_t take = feed(live, 3 * kRate, 4.0);
    finish(live);
    bool kept = true;
    const std::string why = missing(live, take, &kept);
    test::check("V55 a take Live came into late is not kept", !kept);
    test::check("V55 and the reason says where it came in",
                why.find("00:03") != std::string::npos, why);
}

void a_take_live_was_off_for_part_of_is_not() {
    LiveTranscriber live;
    live.begin(engine(), kRate);
    feed(live, 0, 2.0);
    live.finish();           // switched off...
    live.join();
    live.begin(engine(), kRate);   // ...and on again three seconds later
    const std::size_t take = feed(live, 5 * kRate, 2.0);
    finish(live);
    bool kept = true;
    const std::string why = missing(live, take, &kept);
    test::check("V55 a take with Live off in the middle is not kept", !kept);
    test::check("V55 and the reason is the hole", why.find("part of the take") != std::string::npos,
                why);
}

void a_take_live_was_switched_off_before_the_end_of_is_not() {
    LiveTranscriber live;
    live.begin(engine(), kRate);
    const std::size_t heard = feed(live, 0, 3.0);
    finish(live);
    bool kept = true;
    const std::string why = missing(live, heard + 2 * kRate, &kept);
    test::check("V55 a take that ran on after Live went off is not kept", !kept);
    test::check("V55 and the reason says it went off", why.find("before the take ended") !=
                                                           std::string::npos, why);
}

void a_preview_that_failed_is_not_kept() {
    LiveTranscriber live;
    live.begin(engine(0, /*fail_to_load=*/true), kRate);
    const std::size_t take = feed(live, 0, 2.0);
    finish(live);
    bool kept = true;
    const std::string why = missing(live, take, &kept);
    test::check("V55 a preview whose model failed is not kept", !kept);
    test::check("V55 and the reason carries the error",
                why.find("would not load") != std::string::npos, why);
}

void a_preview_stopped_before_it_finished_is_not_kept() {
    LiveTranscriber live;
    // A slow model: the take is over long before it has caught up.
    live.begin(engine(/*decode_ms=*/300), kRate);
    const std::size_t take = feed(live, 0, 8.0);
    live.finish();
    live.stop();             // Cancel, or a transcription taking the model
    live.join();
    bool kept = true;
    const std::string why = missing(live, take, &kept);
    test::check("V55 a preview stopped with audio it never decoded is not kept", !kept, why);
}

void a_preview_that_skipped_ahead_is_not_kept() {
    pipeline::LiveTuning tuning;
    tuning.max_backlog = 2.0;   // far behind after two seconds
    LiveTranscriber live(tuning);
    live.begin(engine(/*decode_ms=*/200), kRate);
    std::size_t at = 0;
    for (int i = 0; i < 12; ++i) {   // faster than it can decode
        at = feed(live, at, 1.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    finish(live);
    bool kept = true;
    const std::string why = missing(live, at, &kept);
    test::check("V55 a preview that skipped ahead is not kept", !kept);
    test::check("V55 and the reason says it fell behind",
                why.find("fell behind") != std::string::npos, why);
}

void a_take_recorded_with_live_off_has_nothing_to_explain() {
    LiveTranscriber live;
    bool kept = true;
    const std::string why = missing(live, 5 * kRate, &kept);
    test::check("V55 Live off: nothing heard, nothing kept",
                !live.heard_take() && !kept && why.empty(), why);
}

void a_new_take_starts_with_a_clean_record() {
    LiveTranscriber live;
    live.begin(engine(), kRate);
    feed(live, 3 * kRate, 2.0);   // a late start, on the take before
    finish(live);
    live.clear();                 // a new take
    live.begin(engine(), kRate);
    const std::size_t take = feed(live, 0, 3.0);
    finish(live);
    std::string why;
    test::check("V55 the last take's gaps do not count against the next",
                live.kept_transcript(take, &why).has_value(), why);
}

}  // namespace

int main() {
    a_take_heard_whole_is_kept();
    a_take_live_missed_the_start_of_is_not();
    a_take_live_was_off_for_part_of_is_not();
    a_take_live_was_switched_off_before_the_end_of_is_not();
    a_preview_that_failed_is_not_kept();
    a_preview_stopped_before_it_finished_is_not_kept();
    a_preview_that_skipped_ahead_is_not_kept();
    a_take_recorded_with_live_off_has_nothing_to_explain();
    a_new_take_starts_with_a_clean_record();
    return test::summary("live");
}
