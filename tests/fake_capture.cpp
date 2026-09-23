// A deterministic AudioCapture, linked in place of src/audio/capture.cpp.
//
// AudioCapture is a pimpl, so this only has to match the header: the class's
// Impl is private and defined per translation unit, and nothing outside
// capture.cpp ever sees the real one. That makes the audio device the one seam
// where a test can stand in for the hardware without the application knowing.

#include "audio/capture.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>

#include "fake_capture.h"

namespace {

struct Stream {
    double silent_for = 0.0;
    double mark_at = 0.0;
    float  marker = 0.0f;
};

struct Control {
    std::mutex mutex;

    std::function<void()> start_hook;
    std::function<void()> stop_hook;
    std::string           start_failure;
    std::string           error;
    std::size_t           total_samples = 0;
    std::map<std::string, std::size_t> source_samples;
    std::map<std::string, Stream>      streams;

    int starts = 0;
    int stops  = 0;
    int running = 0;
};

Control& control() {
    static Control c;
    return c;
}

}  // namespace

namespace fake_capture {

void reset() {
    Control& c = control();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.start_hook = nullptr;
    c.stop_hook  = nullptr;
    c.start_failure.clear();
    c.error.clear();
    c.total_samples = 0;
    c.source_samples.clear();
    c.streams.clear();
    c.starts = c.stops = c.running = 0;
}

void on_start(std::function<void()> hook) {
    Control& c = control();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.start_hook = std::move(hook);
}

void on_stop(std::function<void()> hook) {
    Control& c = control();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.stop_hook = std::move(hook);
}

void fail_start(const std::string& message) {
    Control& c = control();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.start_failure = message;
}

void set_total_samples(std::size_t n) {
    Control& c = control();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.total_samples = n;
}

void set_source_samples(const std::string& source_id, std::size_t n) {
    Control& c = control();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.source_samples[source_id] = n;
}

void set_source_stream(const std::string& source_id, double silent_for,
                       double mark_at, float marker) {
    Control& c = control();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.streams[source_id] = Stream{silent_for, mark_at, marker};
}

void set_error(const std::string& message) {
    Control& c = control();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.error = message;
}

int starts() {
    Control& c = control();
    std::lock_guard<std::mutex> lock(c.mutex);
    return c.starts;
}

int stops() {
    Control& c = control();
    std::lock_guard<std::mutex> lock(c.mutex);
    return c.stops;
}

bool any_running() {
    Control& c = control();
    std::lock_guard<std::mutex> lock(c.mutex);
    return c.running > 0;
}

}  // namespace fake_capture

namespace transcriptor::audio {

struct AudioCapture::Impl {
    bool        open = false;
    bool        drained = false;   // the one block has already been handed over
    std::size_t remaining = 0;

    bool        streaming = false;
    Stream      stream;
    std::chrono::steady_clock::time_point opened;
    std::size_t emitted = 0;       // samples handed over, counted from start()
};

AudioCapture::AudioCapture(AudioSource source, int samplerate)
    : source_(std::move(source)), samplerate_(samplerate),
      impl_(std::make_shared<Impl>()) {}

AudioCapture::~AudioCapture() { stop(); }

void AudioCapture::start() {
    std::function<void()> hook;
    std::string failure;
    {
        Control& c = control();
        std::lock_guard<std::mutex> lock(c.mutex);
        hook = c.start_hook;
        failure = c.start_failure;
        const auto own = c.source_samples.find(source_.id);
        impl_->remaining =
            own != c.source_samples.end() ? own->second : c.total_samples;
        const auto stream = c.streams.find(source_.id);
        impl_->streaming = stream != c.streams.end();
        if (impl_->streaming) impl_->stream = stream->second;
    }
    // Outside the lock: a hook is there precisely so the test can drive this
    // AppState from another thread while the open is in progress.
    if (hook) hook();
    if (!failure.empty()) throw std::runtime_error(failure);

    {
        Control& c = control();
        std::lock_guard<std::mutex> lock(c.mutex);
        ++c.starts;
        ++c.running;
    }
    impl_->opened = std::chrono::steady_clock::now();
    impl_->emitted = 0;
    impl_->open = true;
}

void AudioCapture::stop() {
    if (!impl_ || !impl_->open) return;
    impl_->open = false;

    std::function<void()> hook;
    {
        Control& c = control();
        std::lock_guard<std::mutex> lock(c.mutex);
        hook = c.stop_hook;
        ++c.stops;
        --c.running;
    }
    if (hook) hook();
}

std::vector<float> AudioCapture::drain() {
    if (impl_ && impl_->open && impl_->streaming) {
        const Stream& s = impl_->stream;
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - impl_->opened).count();
        if (now < s.silent_for) return {};
        const auto at = [this](double secs) {
            return static_cast<std::size_t>(secs * samplerate_);
        };
        // Nothing is owed for the quiet stretch: a loopback has no frames of
        // it to hand over late.
        const std::size_t from = std::max(impl_->emitted, at(s.silent_for));
        const std::size_t to   = at(now);
        const std::size_t mark = at(s.mark_at);
        std::vector<float> out;
        for (std::size_t k = from; k < to; ++k) {
            out.push_back(k >= mark && k < mark + 160 ? s.marker : 0.0f);
        }
        impl_->emitted = std::max(impl_->emitted, to);
        return out;
    }
    if (!impl_ || impl_->drained || impl_->remaining == 0) return {};
    impl_->drained = true;
    // A recognisable, non-silent signal: a take of the right length that is all
    // zeroes would pass a size check while telling nothing about the path it
    // travelled.
    std::vector<float> out(impl_->remaining);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = 0.25f * ((i % 200) < 100 ? 1.0f : -1.0f);
    }
    impl_->remaining = 0;
    return out;
}

bool AudioCapture::running() const { return impl_ && impl_->open; }

std::string AudioCapture::error() const {
    Control& c = control();
    std::lock_guard<std::mutex> lock(c.mutex);
    return c.error;
}

}  // namespace transcriptor::audio
