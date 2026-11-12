#include "audio/tap_convert.h"

#include <cmath>
#include <stdexcept>

#include <miniaudio.h>

namespace transcriptor::audio {

namespace {

// Anything outside this is a format we misread, not a device.
ma_uint32 checked_rate(double rate) {
    if (!(rate >= 8000.0 && rate <= 768000.0)) {
        throw std::runtime_error("unusable sample rate");
    }
    return static_cast<ma_uint32>(std::lround(rate));
}

}  // namespace

struct TapConverter::Impl {
    ma_linear_resampler resampler{};
    bool                ready = false;
    ma_uint32           in_rate = 0;
    ma_uint32           out_rate = 0;
    std::vector<float>  mono;

    ~Impl() {
        if (ready) ma_linear_resampler_uninit(&resampler, nullptr);
    }
};

TapConverter::TapConverter(double in_rate, int out_rate) : impl_(std::make_unique<Impl>()) {
    impl_->in_rate  = checked_rate(in_rate);
    impl_->out_rate = checked_rate(out_rate);

    // miniaudio's linear resampler low-passes before it decimates, which a
    // bare linear interpolation would not: 48 kHz down to 16 kHz folds
    // everything above 8 kHz back into the speech band otherwise.
    const ma_linear_resampler_config cfg = ma_linear_resampler_config_init(
        ma_format_f32, 1, impl_->in_rate, impl_->out_rate);
    if (ma_linear_resampler_init(&cfg, nullptr, &impl_->resampler) != MA_SUCCESS) {
        throw std::runtime_error("the resampler could not be created");
    }
    impl_->ready = true;
}

TapConverter::~TapConverter() = default;

double TapConverter::in_rate() const { return impl_->in_rate; }

void TapConverter::set_in_rate(double in_rate) {
    const ma_uint32 rate = checked_rate(in_rate);
    if (rate == impl_->in_rate) return;
    if (ma_linear_resampler_set_rate(&impl_->resampler, rate, impl_->out_rate) != MA_SUCCESS) {
        throw std::runtime_error("the resampler could not change rate");
    }
    impl_->in_rate = rate;
}

void TapConverter::process(const Buffer* buffers, std::size_t buffer_count,
                           std::size_t frames, std::vector<float>* out) {
    if (!buffers || !out || frames == 0) return;

    unsigned channels = 0;
    for (std::size_t b = 0; b < buffer_count; ++b) {
        if (buffers[b].data) channels += buffers[b].channels;
    }
    if (channels == 0) return;

    std::vector<float>& mono = impl_->mono;
    mono.assign(frames, 0.0f);
    for (std::size_t b = 0; b < buffer_count; ++b) {
        const float*   data = buffers[b].data;
        const unsigned n    = buffers[b].channels;
        if (!data || n == 0) continue;
        for (std::size_t f = 0; f < frames; ++f) {
            float sum = 0.0f;
            for (unsigned c = 0; c < n; ++c) sum += data[f * n + c];
            mono[f] += sum;
        }
    }
    const float scale = 1.0f / static_cast<float>(channels);
    for (float& s : mono) s *= scale;

    // The resampler keeps its filter state and its fractional position between
    // calls, so consecutive cycles join without a click or a dropped frame.
    float       chunk[1024];
    std::size_t done = 0;
    while (done < frames) {
        ma_uint64 in_n  = frames - done;
        ma_uint64 out_n = sizeof(chunk) / sizeof(chunk[0]);
        if (ma_linear_resampler_process_pcm_frames(&impl_->resampler, mono.data() + done,
                                                   &in_n, chunk, &out_n) != MA_SUCCESS) {
            return;
        }
        out->insert(out->end(), chunk, chunk + out_n);
        done += static_cast<std::size_t>(in_n);
        if (in_n == 0 && out_n == 0) break;   // cannot make progress; never spin
    }
}

}  // namespace transcriptor::audio
