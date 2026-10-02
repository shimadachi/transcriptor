// Record a whole session into one in-memory buffer.
//
// Offline-first: the transcript is made from this buffer once the take ends.
// A drain thread pulls from the capture queue(s), tracks a level for the meter,
// and appends to the session buffer -- and hands each block to a tap as well,
// when one is attached, which is how the live transcript hears the take. An
// optional second source (a microphone) is mixed into the primary (system
// loopback) in real time with per-source gain and a peak limiter.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/capture.h"

namespace transcriptor::audio {

class Recorder {
public:
    Recorder(AudioSource source, int samplerate,
             std::optional<AudioSource> mic_source = std::nullopt,
             float system_gain = 1.0f, float mic_gain = 1.0f);
    ~Recorder();

    // Throws std::runtime_error if the primary source cannot be opened.
    void start();

    // Stops both streams, flushes what is queued, and returns the session audio.
    std::vector<float> stop();

    void pause();
    void resume();

    bool  paused() const { return paused_.load(); }
    bool  mixing() const { return mic_capture_ != nullptr; }
    float level() const { return level_.load(); }     // RMS of the last block
    double elapsed() const;                            // seconds, excluding pauses

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
    std::vector<float>   samples_;          // guarded by mutex_
    Tap                  tap_;              // guarded by mutex_
    std::vector<float>   carry_[2];         // drain thread only
    // When each source last delivered anything; drain thread only.
    std::chrono::steady_clock::time_point last_got_[2]{};

    std::chrono::steady_clock::time_point started_at_{};
    std::chrono::steady_clock::time_point paused_at_{};
    std::atomic<double>  paused_total_{0.0};
};

}  // namespace transcriptor::audio
