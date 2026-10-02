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
    //
    // `vad_model_path` is the Silero detector that keeps silence away from the
    // decoder, which is what stops it hallucinating subtitle credits there. An
    // empty path transcribes the whole recording, the way this used to.
    //
    // Throws std::runtime_error on load or decode failure.
    std::vector<TranscriptSegment> transcribe(const std::vector<float>& audio,
                                              const paths::fs::path& model_path,
                                              const paths::fs::path& vad_model_path,
                                              const ProgressFn& progress);

    // Frees the model (and its GPU memory). Safe to call when not loaded.
    void unload();

    bool loaded() const;

    const std::string& model_name() const { return model_name_; }

    // Makes an in-flight transcribe() return early.
    void request_abort() { abort_.store(true); }

    // Clears a previous run's abort. Deliberately not done by transcribe()
    // itself: a shutdown raised just before the worker got here was then
    // erased, and the join behind it waited out the whole transcription.
    void reset_abort() { abort_.store(false); }

private:
    struct Impl;

    std::string           model_name_;
    DeviceInfo            device_;
    std::string           language_;   // empty = auto-detect
    int                   threads_;
    std::atomic<bool>     abort_{false};
    std::unique_ptr<Impl> impl_;
};

// The model behind the live transcript. A context of its own, so the preview
// never shares -- or reloads -- the one the finished transcript is made with,
// and tuned for keeping up rather than for the last word of accuracy: greedy
// decoding instead of a five-way beam, and no temperature fallback, whose
// re-decodes are what turns one slow window into a transcript that falls
// further behind with every window after it. The finished transcript is still
// made the careful way; this is only ever the preview of it.
//
// Used by one thread at a time. pipeline::LiveTranscriber runs its sessions one
// after another, which is what makes that true.
class LiveWhisper {
public:
    LiveWhisper();
    ~LiveWhisper();

    LiveWhisper(const LiveWhisper&) = delete;
    LiveWhisper& operator=(const LiveWhisper&) = delete;

    // Loads `model_path` on `device`, or keeps what is already loaded when it
    // is the same file on the same device. Throws std::runtime_error with a
    // user-facing message.
    void load(const paths::fs::path& model_path, const DeviceInfo& device);

    // Segment times are relative to the first sample of `audio`. `language`
    // empty means detect it -- once: the first window with words in it decides,
    // and the rest of the take is held to that language. Detecting it afresh on
    // every few seconds of audio let one mumbled window flip the preview into
    // another language and back.
    //
    // `abort` makes a decode in flight give up; it then returns nothing.
    std::vector<TranscriptSegment> transcribe(const std::vector<float>& audio,
                                              const std::string& language,
                                              const paths::fs::path& vad_model_path,
                                              int threads,
                                              const std::atomic<bool>* abort);

    // A new take: it may be in another language, so detect that again, and
    // give the voice detector another chance if it failed on the last one.
    void begin_take();

    void unload();
    bool loaded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace transcriptor::stt
