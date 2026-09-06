// Decode an audio/video file to mono float32 at a target sample rate.
//
// miniaudio handles wav/flac/mp3 natively. Anything else (m4a, mp4, ogg, webm,
// opus) falls through to the system ffmpeg. All local; nothing is uploaded.
#pragma once

#include <string>
#include <vector>

#include "util/net.h"
#include "util/paths.h"

namespace transcriptor::audio {

// Throws std::runtime_error with a user-facing message on failure.
//
// `cancel` is checked between chunks and kills the ffmpeg child, so an upload
// the user gave up on stops here rather than at the end of the file. A long
// recording spends real time in this call, and it used to be the one stretch of
// a job that Cancel could neither reach nor be remembered across.
std::vector<float> decode_file(const paths::fs::path& path, int samplerate,
                               net::Canceller* cancel = nullptr);

}  // namespace transcriptor::audio
