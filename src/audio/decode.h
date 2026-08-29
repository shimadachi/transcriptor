// Decode an audio/video file to mono float32 at a target sample rate.
//
// miniaudio handles wav/flac/mp3 natively. Anything else (m4a, mp4, ogg, webm,
// opus) falls through to the system ffmpeg. All local; nothing is uploaded.
#pragma once

#include <string>
#include <vector>

#include "util/paths.h"

namespace transcriptor::audio {

// Throws std::runtime_error with a user-facing message on failure.
std::vector<float> decode_file(const paths::fs::path& path, int samplerate);

}  // namespace transcriptor::audio
