// Turns what a macOS system-audio tap delivers into what the recorder wants:
// mono float32 at the recording rate.
//
// miniaudio does this for every other source, because it owns those devices.
// A tap is not a device miniaudio can open, so its I/O block hands over raw
// CoreAudio buffers -- stereo at the output device's rate, 44.1 or 48 kHz --
// and the conversion has to happen here. Kept free of anything Apple so it is
// compiled and tested on every platform, not only where a tap exists.
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace transcriptor::audio {

class TapConverter {
public:
    // Throws std::runtime_error when either rate is not a usable sample rate.
    TapConverter(double in_rate, int out_rate);
    ~TapConverter();

    TapConverter(const TapConverter&) = delete;
    TapConverter& operator=(const TapConverter&) = delete;

    // One CoreAudio buffer: `channels` samples per frame, interleaved.
    struct Buffer {
        const float* data = nullptr;
        unsigned     channels = 0;
    };

    // One I/O cycle. CoreAudio may hand the channels over interleaved in one
    // buffer or one buffer per channel; either way every channel of every
    // buffer is averaged into the mono signal. Appends to `out`.
    void process(const Buffer* buffers, std::size_t buffer_count, std::size_t frames,
                 std::vector<float>* out);

    // The tap follows the output device, and that changes rate when the user
    // moves from the speakers to a headset mid-call. Without this the rest of
    // the take would be resampled from the wrong rate: off-pitch, and drifting
    // against the microphone.
    void set_in_rate(double in_rate);

    double in_rate() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace transcriptor::audio
