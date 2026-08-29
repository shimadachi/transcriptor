// Orchestrate offline processing: transcribe -> diarize -> attribute speakers.
//
// Holds the heavy model objects and reuses them across runs, and can release
// them on demand so the summarizer gets the GPU to itself.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "device.h"
#include "diarize/diarizer.h"
#include "stt/whisper_stt.h"

namespace transcriptor::pipeline {

// phase is one of "transcribe" / "diarize" / "attribute"; fraction is [0,1] or
// -1 when unknown; message overrides the phase's default text when non-empty.
using ProgressFn = std::function<void(const std::string& phase, double fraction,
                                      const std::string& message)>;

struct Line {
    int         speaker = -1;   // 0-based speaker index, -1 = unknown
    std::string text;
    double      start = 0.0;
    double      end   = 0.0;
};

// Seconds -> compact timestamp: mm:ss, or h:mm:ss past an hour.
std::string fmt_ts(double seconds);

struct ProcessResult {
    std::vector<Line> lines;
    int    num_speakers = 0;
    double duration     = 0.0;
    bool   diarized     = false;

    // Readable transcript. `with_ts` prefixes each line with [mm:ss]. The LLM
    // is fed the no-timestamp form; files and the on-screen transcript use the
    // timestamped one.
    std::string plain_text(const std::string& lang = "tr",
                           bool with_ts = false) const;

    nlohmann::json to_json(const std::string& lang = "tr") const;
};

class OfflineProcessor {
public:
    OfflineProcessor(Settings settings, DeviceInfo device);
    ~OfflineProcessor();

    OfflineProcessor(const OfflineProcessor&) = delete;
    OfflineProcessor& operator=(const OfflineProcessor&) = delete;

    // Throws std::runtime_error with a user-facing message on failure.
    ProcessResult run(const std::vector<float>& audio, int samplerate,
                      const ProgressFn& progress);

    // Release Whisper + diarization memory (rebuilt lazily on the next run).
    void unload();

    void request_abort();

private:
    Settings   settings_;
    DeviceInfo device_;

    std::unique_ptr<stt::WhisperTranscriber> transcriber_;
    std::unique_ptr<diarize::Diarizer>       diarizer_;
};

// Assign each word (or whole segment, when word timestamps are missing) to the
// speaker turn it overlaps most, then merge consecutive same-speaker tokens.
std::vector<Line> attribute(const std::vector<stt::TranscriptSegment>& segments,
                            const std::vector<diarize::Turn>& turns,
                            int* num_speakers);

}  // namespace transcriptor::pipeline
