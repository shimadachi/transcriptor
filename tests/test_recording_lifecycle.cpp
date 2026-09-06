// Regression tests for the recording lifecycle (R1, R4, G6, N6, and the
// original device-disconnection finding).
//
// This is where the expensive bugs live: everything here is about the moment a
// take exists only in memory, and every bug in this area ended with a recording
// that could not be reached, could not be stopped, or was silently thrown away.
//
// They run against the real AppState and Recorder. Only the audio device is
// substituted (tests/fake_capture.cpp), which is what makes the races
// reproducible: opening and closing a device is slow, all of these bugs live in
// that gap, and a real sound card gives no way to hold it open.
//
// No model is ever loaded: auto_transcribe is off, so a take stops at "ready"
// and nothing asks Whisper for anything.

#include "app/state.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/sources.h"
#include "check.h"
#include "config.h"
#include "fake_capture.h"
#include "util/export.h"
#include "util/paths.h"

using namespace transcriptor;
using namespace std::chrono_literals;

namespace {

// One-shot latch. Every wait is bounded, so a fix that deadlocks fails the test
// instead of hanging the suite.
class Gate {
public:
    void signal() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }
    bool wait(std::chrono::milliseconds limit = 5000ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, limit, [this] { return open_; });
    }

private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    bool                    open_ = false;
};

paths::fs::path g_scratch;
int             g_case = 0;

// A private output folder per case, so one case cannot see another's files.
paths::fs::path fresh_output_dir() {
    const paths::fs::path dir = g_scratch / ("case" + std::to_string(++g_case));
    std::error_code ec;
    paths::fs::create_directories(dir, ec);
    return dir;
}

Settings test_settings(const paths::fs::path& output_dir) {
    Settings s;
    s.output_dir      = paths::to_utf8(output_dir);
    s.samplerate      = 16000;
    s.save_audio      = true;
    s.save_transcript = false;
    s.save_summary    = false;
    s.auto_transcribe = false;   // nothing here should reach a model
    s.auto_summarize  = false;
    s.enable_diarization = false;
    s.device          = "cpu";
    return s;
}

audio::AudioSource test_source() {
    audio::AudioSource src;
    src.id          = "fake:loopback";
    src.name        = "Fake loopback";
    src.is_loopback = true;
    return src;
}

// Half a second at 16 kHz is the shortest take begin() will accept; use two.
constexpr std::size_t kTakeSamples = 16000;

paths::fs::path saved_audio_in(const paths::fs::path& root) {
    std::error_code ec;
    for (const auto& entry : paths::fs::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) continue;
        const paths::fs::path file = entry.path() / "audio.wav";
        if (paths::fs::is_regular_file(file, ec)) return file;
    }
    return {};
}

std::uintmax_t size_of(const paths::fs::path& p) {
    std::error_code ec;
    return p.empty() ? 0 : paths::fs::file_size(p, ec);
}

// Lets the recorder's drain thread pick up the block the fake device queued.
void let_audio_arrive() { std::this_thread::sleep_for(200ms); }

bool json_bool(const nlohmann::json& j, const char* key) {
    return j.contains(key) && j[key].is_boolean() && j[key].get<bool>();
}

// ---------------------------------------------------------------------------

// R1: Cancel used to complete while a take was still opening its device. It saw
// no published recorder, cleared the flag and reported success -- and startup
// then handed a live capture to an app that believed it was idle. Every route
// checks recording() first, so nothing could stop it afterwards.
void test_cancel_during_device_open() {
    const paths::fs::path out = fresh_output_dir();
    fake_capture::reset();
    fake_capture::set_total_samples(kTakeSamples);

    Gate opening, release;
    fake_capture::on_start([&] {
        opening.signal();
        release.wait();
    });

    app::AppState state(test_settings(out));

    std::thread starter([&] {
        try { state.start_recording(test_source(), {}); } catch (const std::exception&) {}
    });
    test::check("R1 the device open is reached", opening.wait());

    // Cancel arrives while the device is still opening.
    std::thread canceller([&] { state.cancel(); });
    std::this_thread::sleep_for(100ms);   // let cancel get as far as it can
    release.signal();
    starter.join();
    canceller.join();

    test::check("R1 cancel during startup leaves the app idle",
                state.phase() == "idle" && !state.recording(),
                "phase=" + state.phase() +
                    " recording=" + (state.recording() ? "1" : "0"));
    test::check("R1 no capture device is left open", !fake_capture::any_running(),
                std::to_string(fake_capture::starts()) + " opened, " +
                    std::to_string(fake_capture::stops()) + " closed");
    test::check("R1 the cancelled take is not held for transcription",
                !json_bool(state.state_json(), "has_audio"));
}

// G6: the same race at the other end. stop_and_process() holds the lifecycle
// lock only while taking the recorder; during the device close that follows,
// Cancel would run the whole way through -- and the stop then committed the
// take anyway, putting a cancelled recording back on screen and on disk.
void test_cancel_during_device_close() {
    const paths::fs::path out = fresh_output_dir();
    fake_capture::reset();
    fake_capture::set_total_samples(kTakeSamples);

    app::AppState state(test_settings(out));
    state.start_recording(test_source(), {});
    let_audio_arrive();

    Gate closing, release;
    fake_capture::on_stop([&] {
        closing.signal();
        release.wait();
    });

    std::thread stopper([&] { state.stop_and_process(); });
    test::check("G6 the device close is reached", closing.wait());

    state.cancel();   // completes while the device is still closing
    release.signal();
    stopper.join();

    test::check("G6 a take cancelled during Stop is not resurrected",
                state.phase() == "idle" &&
                    !json_bool(state.state_json(), "has_audio"),
                "phase=" + state.phase());
    test::check("G6 the cancelled take is not written to disk",
                saved_audio_in(out).empty());
    test::check("G6 no capture device is left open", !fake_capture::any_running());
}

// R4: normal window close ran shutdown(), which stopped the recorder and threw
// away the samples it returned. Saving only ever happened in begin(), which
// this path never reached -- so closing during a take lost everything captured,
// default save_audio and all.
void test_shutdown_saves_the_take() {
    const paths::fs::path out = fresh_output_dir();
    fake_capture::reset();
    fake_capture::set_total_samples(kTakeSamples);

    {
        app::AppState state(test_settings(out));
        state.start_recording(test_source(), {});
        let_audio_arrive();
        state.shutdown();   // what closing the window does
    }

    const paths::fs::path file = saved_audio_in(out);
    test::check("R4 closing during a recording writes the take out",
                !file.empty(), paths::to_utf8(file));
    test::check("R4 the whole take is there, not a fragment",
                size_of(file) == 44 + kTakeSamples * 2,
                std::to_string(size_of(file)) + " bytes");
    test::check("R4 no capture device is left open", !fake_capture::any_running());
}

// N6: nothing new may start once teardown has begun. start_recording() checked
// processing_ but never shutting_down_, so a request arriving during the close
// opened a device that shutdown had already finished tearing down.
void test_no_take_starts_during_shutdown() {
    const paths::fs::path out = fresh_output_dir();
    fake_capture::reset();
    fake_capture::set_total_samples(kTakeSamples);

    app::AppState state(test_settings(out));
    state.shutdown();

    bool refused = false;
    try {
        state.start_recording(test_source(), {});
    } catch (const app::BusyError&) {
        refused = true;
    } catch (const std::exception&) {}

    test::check("N6 a recording is refused once the app is closing", refused);
    test::check("N6 no device was opened during teardown",
                fake_capture::starts() == 0 && !fake_capture::any_running());
}

// G6, second half: job admission and recording were separate atomics, so a
// request that passed its HTTP check just before a recording started could
// still claim the job and clear the session out from under the take.
void test_job_admission_is_refused_while_recording() {
    const paths::fs::path out = fresh_output_dir();
    fake_capture::reset();
    fake_capture::set_total_samples(kTakeSamples);

    // A real file, so that without the fix this would get as far as decoding.
    const paths::fs::path upload = out / "upload.wav";
    exporter::save_audio_wav(upload, std::vector<float>(kTakeSamples, 0.1f), 16000);

    app::AppState state(test_settings(out));
    state.start_recording(test_source(), {});
    let_audio_arrive();

    const bool accepted = state.process_file(upload, "upload.wav");

    test::check("an upload is refused while a recording is live", !accepted);
    test::check("the live take survives the refused upload",
                state.recording() && state.phase() == "recording",
                "phase=" + state.phase());

    std::string error;
    test::check("Transcribe is refused while a recording is live",
                !state.start_transcribe(&error));
    state.cancel();
}

// The ordinary path, which is what a fix that is too strict would break.
void test_normal_take_is_kept_and_saved() {
    const paths::fs::path out = fresh_output_dir();
    fake_capture::reset();
    fake_capture::set_total_samples(kTakeSamples);

    app::AppState state(test_settings(out));
    state.start_recording(test_source(), {});
    test::check("a take reports itself as recording",
                state.recording() && state.phase() == "recording");
    let_audio_arrive();
    state.stop_and_process();

    const nlohmann::json snapshot = state.state_json();
    test::check("a stopped take is ready to transcribe",
                state.phase() == "ready" && json_bool(snapshot, "has_audio"),
                "phase=" + state.phase());
    test::check("a stopped take is written to disk",
                size_of(saved_audio_in(out)) == 44 + kTakeSamples * 2,
                std::to_string(size_of(saved_audio_in(out))) + " bytes");
    test::check("nothing is reported as unsaved", snapshot["save_error"].is_null());
    test::check("the device is closed once the take ends",
                !fake_capture::any_running() && fake_capture::stops() == 1);
}

// The original finding 4: a device that dies mid-take used to take the whole
// recording with it, because the error was reported and the samples returned
// alongside it were dropped on the floor.
void test_device_failure_keeps_what_was_captured() {
    const paths::fs::path out = fresh_output_dir();
    fake_capture::reset();
    fake_capture::set_total_samples(kTakeSamples);

    app::AppState state(test_settings(out));
    state.start_recording(test_source(), {});
    let_audio_arrive();
    fake_capture::set_error("The audio device stopped unexpectedly.");
    state.stop_and_process();

    const nlohmann::json snapshot = state.state_json();
    test::check("a device failure is reported", state.phase() == "error",
                "phase=" + state.phase() + " message=" + state.message());
    test::check("the audio captured before the failure is kept",
                json_bool(snapshot, "has_audio"));
    test::check("the audio captured before the failure is saved",
                size_of(saved_audio_in(out)) == 44 + kTakeSamples * 2);
}

// A device that will not open must hand the claim back, or the app is stuck
// "recording" for ever with nothing behind it.
void test_failed_device_open_releases_the_claim() {
    const paths::fs::path out = fresh_output_dir();
    fake_capture::reset();
    fake_capture::fail_start("The audio device could not be opened.");

    app::AppState state(test_settings(out));
    bool threw = false;
    try {
        state.start_recording(test_source(), {});
    } catch (const std::exception&) { threw = true; }

    test::check("a device that cannot open reports it", threw);
    test::check("a failed open does not leave the app recording",
                !state.recording() && !fake_capture::any_running());

    // ...and the next attempt is allowed through.
    fake_capture::reset();
    fake_capture::set_total_samples(kTakeSamples);
    bool second = true;
    try {
        state.start_recording(test_source(), {});
    } catch (const std::exception&) { second = false; }
    test::check("a later attempt still works", second && state.recording());
    state.cancel();
}

// An optional feature has to actually reach the code that implements it.
// Moving the sources into an object library once left the feature defines
// behind on the executable: diarize/diarizer.cpp compiles itself out when its
// define is missing, so speaker separation vanished from a build that still
// configured, linked, ran and advertised it everywhere else. Nothing failed --
// the binary just quietly got 26 MB smaller and lost a feature.
//
// The reference here is TRANSCRIPTOR_EXPECT_DIARIZE, handed over by the test's
// own CMakeLists straight from the `TRANSCRIPTOR_DIARIZE` option. Checking
// against TRANSCRIPTOR_HAVE_DIARIZE instead would be circular: that define
// travels with the code under test, so the two could never disagree -- which is
// exactly what a first attempt at this test did, and it passed while the
// feature was missing.
void test_build_features_reach_their_code() {
    const paths::fs::path out = fresh_output_dir();
    fake_capture::reset();
    app::AppState state(test_settings(out));

    const bool built    = state.state_json()["diar_supported"].get<bool>();
    const bool intended = TRANSCRIPTOR_EXPECT_DIARIZE != 0;
    test::check("speaker separation is built exactly when the build asks for it",
                built == intended,
                std::string("option=") + (intended ? "ON" : "OFF") +
                    ", compiled in=" + (built ? "yes" : "no"));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_recording_lifecycle <scratch-dir>\n");
        return 2;
    }
    g_scratch = paths::from_utf8(argv[1]);
    std::error_code ec;
    paths::fs::remove_all(g_scratch, ec);
    paths::fs::create_directories(g_scratch, ec);

    // Keep the real config and models directories out of this entirely: the
    // state snapshot stats the models dir, and nothing here should be able to
    // read or write the machine's actual settings.
    const paths::fs::path home = g_scratch / "home";
    paths::fs::create_directories(home, ec);
#ifndef _WIN32
    setenv("XDG_CONFIG_HOME", paths::to_utf8(home).c_str(), 1);
    setenv("TRANSCRIPTOR_MODELS_DIR", paths::to_utf8(home / "models").c_str(), 1);
#endif

    test_cancel_during_device_open();
    test_cancel_during_device_close();
    test_shutdown_saves_the_take();
    test_no_take_starts_during_shutdown();
    test_job_admission_is_refused_while_recording();
    test_normal_take_is_kept_and_saved();
    test_device_failure_keeps_what_was_captured();
    test_failed_device_open_releases_the_claim();
    test_build_features_reach_their_code();

    return test::summary("recording lifecycle");
}
