// Record a session, either straight to disk or into one in-memory buffer.
//
// Offline-first: the transcript is made from the take once it ends. A drain
// thread pulls from the capture queue(s), tracks a level for the meter, and
// either spools each block to a WAV as it arrives or appends it to a session
// buffer -- and hands it to a tap as well, when one is attached, which is how
// the live transcript hears the take. An optional second source (a
// microphone) is mixed into the primary (system loopback) in real time with
// per-source gain and a peak limiter.
//
// Prefer spool_to(). A take held in memory grows by 230 MB an hour at 16 kHz
// and reallocates by copying, so a long recording asks the host for several
// gigabytes at once and takes the whole machine into its pagefile; see
// audio/spool.h. The in-memory path stays for takes with nowhere to write: a
// spool that cannot be opened must degrade, not end the recording.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/capture.h"
#include "audio/spool.h"
#include "util/paths.h"

namespace transcriptor::audio {

class Recorder {
public:
    Recorder(AudioSource source, int samplerate,
             std::optional<AudioSource> mic_source = std::nullopt,
             float system_gain = 1.0f, float mic_gain = 1.0f);
    ~Recorder();

    // Write the take to `path` as it is captured, rather than keeping it in
    // memory. Must be called before start(). A spool that cannot be opened is
    // not fatal: the take falls back to memory and spool_error() says why, so
    // a read-only folder costs the user a warning and not the recording.
    void spool_to(const paths::fs::path& path);

    // Throws std::runtime_error if the primary source cannot be opened.
    void start();

    // Stops both streams and flushes what is queued.
    //
    // Spooled, this finishes the file and returns nothing: the take is at
    // spool_path(), and audio::read_wav_f32() is how a caller that needs it in
    // memory gets it there -- in one allocation of exactly its length. Held in
    // memory, this returns the session audio as it always did.
    std::vector<float> stop();

    // Throw the take away: Cancel, whose audio nobody wants. Removes the spool
    // file. Safe before or after stop().
    void discard();

    void pause();
    void resume();

    bool  paused() const { return paused_.load(); }
    bool  mixing() const { return mic_capture_ != nullptr; }
    float level() const { return level_.load(); }     // RMS of the last block
    double elapsed() const;                            // seconds, excluding pauses

    // Where the take is being written, empty when it is being held in memory.
    // A spool that failed to open leaves this empty too, so one check covers
    // both ways of ending up without a file.
    const paths::fs::path& spool_path() const { return spool_path_; }
    bool spooling() const { return !spool_path_.empty(); }

    // Non-empty when the take could not be written: the folder was read-only,
    // the disk filled up, the WAV format ran out of length field. Whatever is
    // already on disk stays valid and playable.
    std::string spool_error() const;

    // Samples kept so far, however they are being kept. The meter and the
    // elapsed clock come from the device; this is what actually landed.
    std::uint64_t captured() const;

    // Primary-stream error; a microphone that dies mid-session degrades to
    // system-only instead of failing the recording.
    std::string error() const;

    // Every block the take keeps, as it is kept -- gain applied, mixed, and
    // nothing from while it was paused: exactly what stop() will return. The
    // offset is where the block starts in the take, in samples, so a listener
    // attached mid-take knows where it came in. Called on the drain thread, and
    // on whichever thread calls stop() for the tail; keep it short. nullptr
    // detaches.
    using Tap = std::function<void(const std::vector<float>& block,
                                   std::size_t offset)>;
    void set_tap(Tap tap);

private:
    void run();
    void run_single();
    void run_mixed();
    bool pump_carry();
    // `flush` evens the two sides up with silence first, for the end of a take.
    bool take_mixed(std::vector<float>* out, bool flush);
    void append(const std::vector<float>& block);

    int   samplerate_;
    float system_gain_;
    float mic_gain_;

    std::unique_ptr<AudioCapture> capture_;
    std::unique_ptr<AudioCapture> mic_capture_;

    std::thread       drain_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> paused_{false};
    std::atomic<float> level_{0.0f};

    mutable std::mutex   mutex_;
    std::vector<float>   samples_;          // guarded by mutex_; unspooled only
    std::unique_ptr<WavSpool> spool_;       // guarded by mutex_
    paths::fs::path      spool_path_;       // set before start(), then read-only
    std::uint64_t        total_ = 0;        // guarded by mutex_; samples kept
    Tap                  tap_;              // guarded by mutex_
    std::vector<float>   carry_[2];         // drain thread only
    // When each source last delivered anything; drain thread only.
    std::chrono::steady_clock::time_point last_got_[2]{};

    std::chrono::steady_clock::time_point started_at_{};
    std::chrono::steady_clock::time_point paused_at_{};
    std::atomic<double>  paused_total_{0.0};
};

}  // namespace transcriptor::audio
