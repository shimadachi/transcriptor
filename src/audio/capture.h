// Records mono float32 blocks from one source into a bounded queue.
//
// miniaudio converts channel count and sample rate for us, so every block that
// comes out is already mono at the configured rate — no resampling here.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/sources.h"

namespace transcriptor::audio {

class AudioCapture {
public:
    AudioCapture(AudioSource source, int samplerate);
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // Throws std::runtime_error when the device cannot be opened.
    void start();
    void stop();

    // Moves every queued block out. Empty when nothing has arrived yet.
    std::vector<float> drain();

    bool running() const;

    // Non-empty once the stream has failed; the recorder surfaces this.
    std::string error() const;

    const AudioSource& source() const { return source_; }

private:
    struct Impl;

    AudioSource           source_;
    int                   samplerate_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace transcriptor::audio
