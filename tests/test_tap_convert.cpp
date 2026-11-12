// Tests for converting what a macOS system-audio tap delivers (M1).
//
// No bug behind these yet: the tap is the one audio source whose samples this
// app converts itself -- miniaudio does it for every device it opens, and a tap
// is not one -- and the only place the real thing runs is a Mac with a user in
// front of it to answer a permission prompt. This is the half that needs
// neither, so it is checked everywhere: the mixdown, the rate, and the property
// a recorder mixing two sources depends on, that no frames go missing between
// one I/O cycle and the next.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio/tap_convert.h"
#include "check.h"

using transcriptor::audio::TapConverter;

namespace {

const double kPi = 3.14159265358979323846;

// `seconds` of a sine, `channels` interleaved, each channel scaled by gain[c].
std::vector<float> tone(double hz, double rate, double seconds, unsigned channels,
                        const float* gain) {
    const std::size_t frames = static_cast<std::size_t>(rate * seconds);
    std::vector<float> out(frames * channels);
    for (std::size_t f = 0; f < frames; ++f) {
        const float s = static_cast<float>(0.5 * std::sin(2.0 * kPi * hz * f / rate));
        for (unsigned c = 0; c < channels; ++c) out[f * channels + c] = s * gain[c];
    }
    return out;
}

// Amplitude of the `hz` component, by correlating against it.
double amplitude_at(const std::vector<float>& x, std::size_t from, double hz, double rate) {
    double re = 0.0, im = 0.0;
    const std::size_t n = x.size() - from;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 2.0 * kPi * hz * i / rate;
        re += x[from + i] * std::cos(w);
        im += x[from + i] * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

double peak(const std::vector<float>& x) {
    double p = 0.0;
    for (float s : x) p = std::max(p, static_cast<double>(std::fabs(s)));
    return p;
}

// Feeds interleaved audio through in `cycle`-frame pieces, the way Core Audio
// hands it over -- 512 frames at a time, not a take at once.
std::vector<float> run(TapConverter& conv, const std::vector<float>& in, unsigned channels,
                       std::size_t cycle) {
    std::vector<float> out;
    const std::size_t frames = in.size() / channels;
    for (std::size_t at = 0; at < frames; at += cycle) {
        const TapConverter::Buffer b{in.data() + at * channels, channels};
        conv.process(&b, 1, std::min(cycle, frames - at), &out);
    }
    return out;
}

std::string num(double v) { return std::to_string(v); }

const float kBoth[2]     = {1.0f, 1.0f};
const float kOpposite[2] = {1.0f, -1.0f};
const float kLeftOnly[2] = {1.0f, 0.0f};

void test_stereo_48k_to_mono_16k() {
    TapConverter conv(48000.0, 16000);
    const auto out = run(conv, tone(440.0, 48000.0, 3.0, 2, kBoth), 2, 512);

    // Three seconds in, three seconds out. The slack is the resampler's filter
    // delay, a handful of samples once -- not something that grows per cycle.
    const long want = 3 * 16000;
    const long got  = static_cast<long>(out.size());
    test::check("48 kHz stereo comes out as the same duration at 16 kHz",
                std::labs(got - want) <= 16, "samples " + std::to_string(got));

    const double a = amplitude_at(out, 1600, 440.0, 16000.0);
    test::check("the tone survives at its own pitch and level", a > 0.45 && a < 0.55,
                "amplitude at 440 Hz " + num(a));
}

void test_no_frames_lost_between_cycles() {
    // The length check above would pass with a click at every boundary. A
    // 440 Hz sine at this level never moves more than 0.087 between two 16 kHz
    // samples; a dropped or repeated frame at a boundary would.
    for (std::size_t cycle : {64u, 441u, 512u, 4096u}) {
        TapConverter conv(48000.0, 16000);
        const auto out = run(conv, tone(440.0, 48000.0, 2.0, 2, kBoth), 2, cycle);
        double worst = 0.0;
        for (std::size_t i = 1601; i < out.size(); ++i) {
            worst = std::max(worst, static_cast<double>(std::fabs(out[i] - out[i - 1])));
        }
        test::check(("cycles of " + std::to_string(cycle) + " frames join without a step").c_str(),
                    worst < 0.10, "largest step " + num(worst));
    }
}

void test_mixdown_reads_every_channel() {
    {
        TapConverter conv(48000.0, 16000);
        const auto out = run(conv, tone(440.0, 48000.0, 1.0, 2, kOpposite), 2, 512);
        test::check("opposite channels cancel, so both were read", peak(out) < 1e-4,
                    "peak " + num(peak(out)));
    }
    {
        // One channel playing is half as loud in mono: an average, not a sum
        // that could clip when both channels are full scale.
        TapConverter conv(48000.0, 16000);
        const auto out = run(conv, tone(440.0, 48000.0, 1.0, 2, kLeftOnly), 2, 512);
        const double a = amplitude_at(out, 1600, 440.0, 16000.0);
        test::check("the mixdown is an average of the channels", a > 0.22 && a < 0.28,
                    "amplitude " + num(a));
    }
}

void test_one_buffer_per_channel() {
    // Core Audio may deliver the channels deinterleaved: two buffers of one
    // channel each instead of one buffer of two.
    const std::size_t frames = 48000;
    const float one = 1.0f;
    const auto left = tone(440.0, 48000.0, 1.0, 1, &one);
    std::vector<float> right(frames);
    for (std::size_t i = 0; i < frames; ++i) right[i] = -left[i];

    TapConverter conv(48000.0, 16000);
    std::vector<float> cancelled;
    for (std::size_t at = 0; at < frames; at += 512) {
        const TapConverter::Buffer b[2] = {{left.data() + at, 1}, {right.data() + at, 1}};
        conv.process(b, 2, std::min<std::size_t>(512, frames - at), &cancelled);
    }
    test::check("deinterleaved channels are mixed like interleaved ones",
                !cancelled.empty() && peak(cancelled) < 1e-4, "peak " + num(peak(cancelled)));

    // A buffer the OS left empty is skipped, not read through a null pointer,
    // and does not count as a channel in the average.
    TapConverter conv2(48000.0, 16000);
    std::vector<float> out;
    for (std::size_t at = 0; at < frames; at += 512) {
        const TapConverter::Buffer b[2] = {{left.data() + at, 1}, {nullptr, 1}};
        conv2.process(b, 2, std::min<std::size_t>(512, frames - at), &out);
    }
    const double a = amplitude_at(out, 1600, 440.0, 16000.0);
    test::check("an empty buffer is ignored", a > 0.45 && a < 0.55, "amplitude " + num(a));
}

void test_44k1_and_a_rate_change() {
    // The other rate an output device runs at.
    TapConverter conv(44100.0, 16000);
    auto out = run(conv, tone(440.0, 44100.0, 2.0, 2, kBoth), 2, 512);
    const long got = static_cast<long>(out.size());
    test::check("44.1 kHz comes out as the same duration", std::labs(got - 32000) <= 16,
                "samples " + std::to_string(got));
    const double a = amplitude_at(out, 1600, 440.0, 16000.0);
    test::check("44.1 kHz keeps its pitch", a > 0.45, "amplitude at 440 Hz " + num(a));

    // The user moves from the speakers to a headset and the tap starts
    // delivering at 48 kHz. Read at the old rate, a 440 Hz tone comes out at
    // 479 Hz and eight percent too long.
    conv.set_in_rate(48000.0);
    test::check("the new rate is the one in use", conv.in_rate() == 48000.0);
    const std::size_t before = out.size();
    const auto more = run(conv, tone(440.0, 48000.0, 2.0, 2, kBoth), 2, 512);
    out.insert(out.end(), more.begin(), more.end());

    const long added = static_cast<long>(out.size() - before);
    test::check("after a rate change the duration is still right",
                std::labs(added - 32000) <= 16, "samples " + std::to_string(added));
    const double right = amplitude_at(out, before + 1600, 440.0, 16000.0);
    const double wrong = amplitude_at(out, before + 1600, 440.0 * 48000.0 / 44100.0, 16000.0);
    test::check("after a rate change the pitch is still right", right > 0.45 && wrong < 0.05,
                "at 440 Hz " + num(right) + ", at 479 Hz " + num(wrong));
}

void test_same_rate_and_bad_input() {
    TapConverter conv(16000.0, 16000);
    const float one = 1.0f;
    const auto in  = tone(440.0, 16000.0, 1.0, 1, &one);
    const auto out = run(conv, in, 1, 512);
    test::check("nothing to resample: every frame comes through",
                std::labs(static_cast<long>(out.size()) - 16000) <= 16,
                "samples " + std::to_string(out.size()));

    std::vector<float> none;
    conv.process(nullptr, 0, 512, &none);
    const TapConverter::Buffer empty{in.data(), 1};
    conv.process(&empty, 1, 0, &none);
    const TapConverter::Buffer no_channels{in.data(), 0};
    conv.process(&no_channels, 1, 512, &none);
    test::check("an empty cycle produces nothing", none.empty());

    // A format that was misread must not become a resampler that spins or
    // divides by zero on the audio thread.
    bool threw = false;
    try {
        TapConverter bad(0.0, 16000);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    test::check("a zero sample rate is refused", threw);

    threw = false;
    try {
        conv.set_in_rate(std::nan(""));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    test::check("a rate change to nonsense is refused, and the old rate kept",
                threw && conv.in_rate() == 16000.0);
}

}  // namespace

int main() {
    test_stereo_48k_to_mono_16k();
    test_no_frames_lost_between_cycles();
    test_mixdown_reads_every_channel();
    test_one_buffer_per_channel();
    test_44k1_and_a_rate_change();
    test_same_rate_and_bad_input();
    return test::summary("tap_convert");
}
