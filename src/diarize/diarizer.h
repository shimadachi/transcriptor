// Offline speaker diarization, fully local.
//
// Replaces pyannote.audio: the same pyannote segmentation-3.0 network, exported
// to ONNX and run through sherpa-onnx, plus a speaker-embedding model and
// clustering. No PyTorch, no HuggingFace token, no network at inference time.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "device.h"

namespace transcriptor::diarize {

struct Turn {
    double start = 0.0;
    double end   = 0.0;
    int    speaker = 0;   // 0-based, dense
};

// Human label for a speaker index; `speaker < 0` means unknown.
std::string speaker_name(int speaker, const std::string& lang = "tr");

using ProgressFn = std::function<void(const std::string& message, double fraction)>;

class Diarizer {
public:
    Diarizer(Settings settings, DeviceInfo device);
    ~Diarizer();

    Diarizer(const Diarizer&) = delete;
    Diarizer& operator=(const Diarizer&) = delete;

    // `audio` is mono float32 at 16 kHz. Throws std::runtime_error on failure.
    std::vector<Turn> diarize(const std::vector<float>& audio, int samplerate,
                              const ProgressFn& progress);

    void unload();

    // False when the build has diarization compiled out.
    static bool supported();

private:
    struct Impl;

    Settings              settings_;
    DeviceInfo            device_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace transcriptor::diarize
