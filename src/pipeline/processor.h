// Orchestrate offline processing: transcribe -> diarize -> attribute speakers.
//
// Holds the heavy model objects and reuses them across runs, and can release
// them on demand so the summarizer gets the GPU to itself.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "device.h"
#include "diarize/diarizer.h"
#include "stt/whisper_stt.h"
#include "util/net.h"

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

    // Also stops a first-run model download in its tracks: without this the
    // Cancel button did nothing until curl had finished fetching gigabytes.
    void request_abort();

    // Clear a previous run's cancellation. Called when a job is admitted, not
    // when run() starts: resetting on the way in erased a shutdown that had
    // already been requested, and the join behind it then waited out a
    // multi-gigabyte model download that nothing was left able to stop.
    void reset_abort();

private:
    // Throws with the cancellation message when an abort is outstanding.
    void throw_if_aborted() const;

    Settings   settings_;
    DeviceInfo device_;

    // Cancellation lives here, at processor scope, not only in the engines it
    // is forwarded to. A cancel raised before run() creates its transcriber had
    // nowhere to be recorded: the download canceller was set, but a model
    // already on disk never consults it, and the brand-new WhisperTranscriber
    // started with a clear flag -- so shutdown was ignored and the load and the
    // inference ran anyway.
    std::atomic<bool> aborted_{false};

    // Kills the curl behind a first-run model fetch.
    net::Canceller dl_cancel_;

    // Guards the two model handles below -- the pointers, not the work done
    // through them. request_abort() runs on whichever thread is cancelling
    // while run() owns the worker, and run() replaces transcriber_ whenever the
    // Whisper model changes: unsynchronized, the cancelling thread could reach
    // into an object make_unique had just freed.
    mutable std::mutex model_mutex_;

    std::unique_ptr<stt::WhisperTranscriber> transcriber_;
    std::unique_ptr<diarize::Diarizer>       diarizer_;
};

// Assign each word (or whole segment, when word timestamps are missing) to the
// speaker turn it overlaps most, then merge consecutive same-speaker tokens.
std::vector<Line> attribute(const std::vector<stt::TranscriptSegment>& segments,
                            const std::vector<diarize::Turn>& turns,
                            int* num_speakers);

}  // namespace transcriptor::pipeline
