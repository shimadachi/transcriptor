// The macOS system-audio tap against the real OS (M1).
//
// Everything else in this directory stands in for the audio device. This is the
// one test that cannot: whether Core Audio accepts the tap, builds the device
// around it and calls back with audio is only answered by Core Audio. So it
// plays a tone through the default output and records it back through the same
// AudioCapture the application uses.
//
// It makes a sound and it needs a permission, so it does nothing unless asked:
//
//   TRANSCRIPTOR_TEST_SYSTEM_AUDIO=1       the tap must be set up and deliver
//                                          audio at the right rate; whether
//                                          the tone was heard is reported
//   TRANSCRIPTOR_TEST_SYSTEM_AUDIO=signal  the tone must be heard as well
//
// "signal" is the full test, and the one to run on a Mac with a person at it.
// The first run puts up the System Audio Recording prompt for whatever launched
// the test (Terminal, an IDE); allow it and run again. The weaker mode exists
// for machines where nobody can answer that prompt: denied, a tap behaves
// exactly as it does when nothing is playing, and delivers silence.
//
// Exits 77 -- skipped, to ctest -- when not asked or when the OS is too old.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <miniaudio.h>

#include "audio/capture.h"
#include "audio/sources.h"
#include "audio/system_tap.h"
#include "check.h"

using namespace transcriptor::audio;
using Clock = std::chrono::steady_clock;

namespace {

const double kPi     = 3.14159265358979323846;
const double kToneHz = 1000.0;
const int    kRate   = 16000;

double seconds_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

void tone_callback(ma_device* device, void* output, const void*, ma_uint32 frames) {
    auto* phase = static_cast<double*>(device->pUserData);
    auto* out   = static_cast<float*>(output);
    const ma_uint32 channels = device->playback.channels;
    const double    step     = 2.0 * kPi * kToneHz / device->sampleRate;
    for (ma_uint32 f = 0; f < frames; ++f) {
        const float s = static_cast<float>(0.2 * std::sin(*phase));
        *phase += step;
        if (*phase > 2.0 * kPi) *phase -= 2.0 * kPi;
        for (ma_uint32 c = 0; c < channels; ++c) out[f * channels + c] = s;
    }
}

double amplitude_at(const std::vector<float>& x, double hz) {
    if (x.empty()) return 0.0;
    double re = 0.0, im = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double w = 2.0 * kPi * hz * i / kRate;
        re += x[i] * std::cos(w);
        im += x[i] * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(x.size());
}

// Waits for the first audio, then collects `seconds` more of it. Returns the
// seconds it took for the first audio to arrive, or a negative number if none
// did within `patience`.
double collect(AudioCapture& capture, double patience, double seconds,
               std::vector<float>* out) {
    const Clock::time_point begin = Clock::now();
    while (seconds_since(begin) < patience) {
        std::vector<float> block = capture.drain();
        if (!block.empty()) break;   // discarded: it only marks the start
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const double waited = seconds_since(begin);
    if (waited >= patience) return -1.0;

    const Clock::time_point from = Clock::now();
    while (seconds_since(from) < seconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::vector<float> block = capture.drain();
        out->insert(out->end(), block.begin(), block.end());
    }
    return waited;
}

std::string num(double v) { return std::to_string(v); }

}  // namespace

int main() {
    const char* asked = std::getenv("TRANSCRIPTOR_TEST_SYSTEM_AUDIO");
    const std::string mode = asked ? asked : "";
    if (mode.empty() || mode == "0") {
        std::printf("skipped: plays a tone and needs the System Audio Recording permission.\n"
                    "         Run with TRANSCRIPTOR_TEST_SYSTEM_AUDIO=signal on a Mac to test it.\n");
        return 77;
    }
    if (!system_tap_available()) {
        std::printf("skipped: this macOS has no system-audio tap (needs 14.2 or newer).\n");
        return 77;
    }
    const bool need_signal = mode == "signal";

    // -- the source is offered, and is what "record system audio" means -------
    std::optional<AudioSource> tap;
    try {
        for (const auto& s : list_sources()) {
            std::printf("     source  %-12s %s%s\n", s.is_loopback ? "[system]" : "[microphone]",
                        s.name.c_str(), s.is_default ? "  (default)" : "");
            if (s.id == kSystemTapSourceId) tap = s;
        }
    } catch (const std::exception& e) {
        test::check("the audio sources can be listed", false, e.what());
        return test::summary("system_audio_mac");
    }
    test::check("the system-audio source is listed", tap.has_value());
    if (!tap) return test::summary("system_audio_mac");
    test::check("it is listed as system audio", tap->is_loopback);

    const auto def = default_loopback_source();
    test::check("it is the default thing to record", def && def->id == kSystemTapSourceId,
                def ? def->id : "no default at all");
    test::check("nobody is told to install BlackHole", macos_loopback_hint().empty(),
                macos_loopback_hint());
    test::check("it can be found again by its id", find_source(kSystemTapSourceId).has_value());

    // What the OS says about the permission, read the way the application
    // reads it. Printed, not checked: every answer is a legitimate one here,
    // and seeing it next to what was heard is how the reading itself is judged.
    static const char* const kPermission[] = {"unknown", "granted", "denied", "not asked",
                                              "pending"};
    std::printf("     permission  %s\n", kPermission[static_cast<int>(system_tap_permission())]);

    // -- something to hear -----------------------------------------------------
    double    phase = 0.0;
    ma_device player{};
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.dataCallback    = tone_callback;
    cfg.pUserData       = &phase;
    bool playing = ma_device_init(nullptr, &cfg, &player) == MA_SUCCESS;
    if (playing && ma_device_start(&player) != MA_SUCCESS) {
        ma_device_uninit(&player);
        playing = false;
    }
    std::printf("     tone    %s\n", playing ? "playing a 1 kHz tone on the default output"
                                             : "NOT playing: no usable output device");

    // -- a take ----------------------------------------------------------------
    std::vector<float> heard;
    {
        AudioCapture capture(*tap, kRate);
        bool started = false;
        try {
            capture.start();
            started = true;
        } catch (const std::exception& e) {
            test::check("the tap starts", false, e.what());
        }
        if (started) {
            test::check("the tap starts", capture.running());

            const double waited = collect(capture, 10.0, 3.0, &heard);
            test::check("audio arrives", waited >= 0.0,
                        waited >= 0.0 ? "first audio after " + num(waited) + " s"
                                      : "nothing in 10 s");

            // A tap read at the wrong rate still "works": the take is merely
            // the wrong length and the wrong pitch. Three seconds of listening
            // has to come out as about three seconds of audio.
            const double got = static_cast<double>(heard.size()) / kRate;
            test::check("it arrives at the recording rate", got > 2.55 && got < 3.45,
                        num(got) + " s of audio in 3 s");
            test::check("the capture reports no error", capture.error().empty(), capture.error());

            const Clock::time_point stopping = Clock::now();
            capture.stop();
            const double took = seconds_since(stopping);
            test::check("it stops, and promptly", !capture.running() && took < 2.5,
                        num(took) + " s");
        }
    }

    // -- was the tone in it? ---------------------------------------------------
    const double tone  = amplitude_at(heard, kToneHz);
    const double other = amplitude_at(heard, 1700.0);
    double peak = 0.0;
    for (float s : heard) peak = std::max(peak, static_cast<double>(std::fabs(s)));
    const bool heard_tone = tone > 0.002 && tone > 5.0 * other;
    std::printf("     heard   peak %.5f, at 1 kHz %.5f, at 1.7 kHz %.5f\n", peak, tone, other);

    if (need_signal) {
        test::check("the tone that was played is in the recording", heard_tone,
                    playing ? "" : "nothing was playing: no output device");
    } else {
        std::printf("%s  the tone that was played is in the recording\n",
                    heard_tone ? "ok  " : "note");
    }
    if (!heard_tone && peak == 0.0) {
        std::printf(
            "\n     The recording is digital silence. A tap delivers exactly that when the\n"
            "     System Audio Recording permission has not been granted to whatever\n"
            "     launched this test -- and equally when nothing is playing, which is why\n"
            "     the application cannot tell the user which one it was.\n"
            "     System Settings > Privacy & Security > Screen & System Audio Recording\n"
            "     > System Audio Recording Only.\n\n");
    }

    // -- and again -------------------------------------------------------------
    // A second take is a second tap and a second aggregate device. If the first
    // one was not torn down completely, this is where it shows.
    {
        AudioCapture again(*tap, kRate);
        try {
            again.start();
            std::vector<float> more;
            const double waited = collect(again, 10.0, 1.0, &more);
            test::check("a second take records too", waited >= 0.0 && !more.empty(),
                        std::to_string(more.size()) + " samples");
            again.stop();
        } catch (const std::exception& e) {
            test::check("a second take records too", false, e.what());
        }
    }

    if (playing) ma_device_uninit(&player);
    return test::summary("system_audio_mac");
}
