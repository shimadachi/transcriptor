// Regression tests for mixing a microphone into system audio (V2).
//
// The bug: the recorder mixed only as many samples as both sources had
// delivered. WASAPI loopback delivers nothing at all while nothing is playing,
// so with the speakers silent the microphone was held back for the whole take
// and dropped at Stop -- an in-person meeting recorded as "system + mic" came
// out empty, and the level meter never moved.
//
// And its sequel (V24): once the speakers did start playing, they were mixed
// in against whatever the microphone had queued meanwhile, and stayed up to a
// second ahead of it for the rest of the take.
//
// Links the real Recorder against the stand-in capture, which can make one
// source silent in exactly the way that loopback is.

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "audio/recorder.h"
#include "check.h"
#include "fake_capture.h"

using transcriptor::audio::AudioSource;
using transcriptor::audio::Recorder;

namespace {

constexpr int kRate = 16000;

const AudioSource kSpeakers{"loop:speakers", "Speakers", true, 2, true};
const AudioSource kHeadset{"mic:headset", "Headset", false, 1, false};

// The drain thread polls every 20 ms; give it a moment to take the block.
bool wait_for_level(const Recorder& rec) {
    for (int i = 0; i < 100 && rec.level() <= 0.0f; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return rec.level() > 0.0f;
}

void mic_is_kept_while_the_speakers_are_silent() {
    fake_capture::reset();
    fake_capture::set_total_samples(3 * kRate);             // the mic speaks
    fake_capture::set_source_samples(kSpeakers.id, 0);      // nothing playing

    Recorder rec(kSpeakers, kRate, kHeadset);
    rec.start();
    test::check("V2 the meter moves while only the mic delivers", wait_for_level(rec));
    const std::vector<float> take = rec.stop();
    test::check("V2 the mic is recorded while the speakers are silent",
                take.size() == static_cast<std::size_t>(3 * kRate),
                std::to_string(take.size()) + " samples");
}

void the_last_words_are_not_left_behind() {
    // Both sources deliver, the mic a little less -- inside the skew allowed
    // while running, so it is only Stop that can even the two up.
    fake_capture::reset();
    fake_capture::set_source_samples(kSpeakers.id, 3 * kRate);
    fake_capture::set_source_samples(kHeadset.id, 3 * kRate - kRate / 2);

    Recorder rec(kSpeakers, kRate, kHeadset);
    rec.start();
    wait_for_level(rec);
    const std::vector<float> take = rec.stop();
    test::check("V2 Stop keeps the tail one source had not been matched against",
                take.size() == static_cast<std::size_t>(3 * kRate),
                std::to_string(take.size()) + " samples");
}

void two_live_sources_still_mix() {
    fake_capture::reset();
    fake_capture::set_total_samples(2 * kRate);

    Recorder rec(kSpeakers, kRate, kHeadset);
    rec.start();
    wait_for_level(rec);
    const std::vector<float> take = rec.stop();
    // Both sources carry the same +/-0.25 square wave, so the sum is +/-0.5.
    test::check("both sources are summed sample for sample",
                take.size() == static_cast<std::size_t>(2 * kRate) &&
                    take.front() == 0.5f,
                std::to_string(take.size()) + " samples, first " +
                    (take.empty() ? "-" : std::to_string(take.front())));
}

void speakers_that_start_playing_line_up_with_the_mic() {
    // V24. Nothing plays for the first 1.5 s, so the loopback sends nothing;
    // the mic runs throughout. At 2 s both hear the same moment -- the
    // speakers mark it 0.5, the mic 0.25. The mix of that moment should hold
    // both, 0.75, in one place. It held them apart: the speakers resumed
    // against whatever the mic had queued since the last second's worth of
    // silence was made up, and stayed that far ahead for the rest of the take.
    fake_capture::reset();
    fake_capture::set_source_stream(kSpeakers.id, 1.5, 2.0, 0.5f);
    fake_capture::set_source_stream(kHeadset.id, 0.0, 2.0, 0.25f);

    Recorder rec(kSpeakers, kRate, kHeadset);
    rec.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const std::vector<float> take = rec.stop();

    auto first = [&take](float lo, float hi) {
        for (std::size_t i = 0; i < take.size(); ++i) {
            if (take[i] > lo && take[i] < hi) return static_cast<long>(i);
        }
        return -1L;
    };
    const long both = first(0.74f, 0.76f);
    const long speakers = both >= 0 ? both : first(0.49f, 0.51f);
    const long mic = both >= 0 ? both : first(0.24f, 0.26f);
    const double apart = std::abs(speakers - mic) / static_cast<double>(kRate);
    test::check("V24 speakers that start playing mid-take line up with the mic",
                speakers >= 0 && mic >= 0 && apart < 0.05,
                "speakers at " + std::to_string(speakers) + ", mic at " +
                    std::to_string(mic) + " (" + std::to_string(apart) + " s apart)");
}

}  // namespace

int main() {
    mic_is_kept_while_the_speakers_are_silent();
    the_last_words_are_not_left_behind();
    two_live_sources_still_mix();
    speakers_that_start_playing_line_up_with_the_mic();
    return test::summary("mixing");
}
