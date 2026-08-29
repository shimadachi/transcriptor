// Accurate offline transcription with whisper.cpp, including word-level
// timestamps (which the diarization step needs to attribute speakers).
//
// Replaces faster-whisper. The model is held until unload() so consecutive
// recordings reuse it, and released before the summarizer loads so a single
// GPU never has to hold both.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "device.h"

namespace transcriptor::stt {

struct Word {
    double      start = 0.0;   // seconds from the start of the recording
    double      end   = 0.0;
    std::string text;          // includes its leading space, as whisper emits it
};

struct TranscriptSegment {
    double            start = 0.0;
    double            end   = 0.0;
    std::string       text;
    std::vector<Word> words;
};

// message may be empty (keep the current one); fraction is [0,1] or -1.
using ProgressFn = std::function<void(const std::string& message, double fraction)>;

class WhisperTranscriber {
public:
    WhisperTranscriber(std::string model_name, DeviceInfo device,
                       std::string language, int threads);
    ~WhisperTranscriber();

    WhisperTranscriber(const WhisperTranscriber&) = delete;
    WhisperTranscriber& operator=(const WhisperTranscriber&) = delete;

    // Loads the model if needed. `audio` is mono float32 at 16 kHz.
    // Throws std::runtime_error on load or decode failure.
    std::vector<TranscriptSegment> transcribe(const std::vector<float>& audio,
                                              const paths::fs::path& model_path,
                                              const ProgressFn& progress);

    // Frees the model (and its GPU memory). Safe to call when not loaded.
    void unload();

    bool loaded() const;

    const std::string& model_name() const { return model_name_; }

    // Makes an in-flight transcribe() return early.
    void request_abort() { abort_.store(true); }

private:
    struct Impl;

    std::string           model_name_;
    DeviceInfo            device_;
    std::string           language_;   // empty = auto-detect
    int                   threads_;
    std::atomic<bool>     abort_{false};
    std::unique_ptr<Impl> impl_;
};

}  // namespace transcriptor::stt
