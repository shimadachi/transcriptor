#include "audio/recorder.h"

#include <algorithm>
#include <cmath>

namespace transcriptor::audio {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

// Peak limiter: scale down only if the summed signal would clip.
void limit(std::vector<float>* block) {
    float peak = 0.0f;
    for (float v : *block) peak = std::max(peak, std::fabs(v));
    if (peak > 1.0f) {
        const float inv = 1.0f / peak;
        for (float& v : *block) v *= inv;
    }
}

float rms(const std::vector<float>& block) {
    if (block.empty()) return 0.0f;
    double acc = 0.0;
    for (float v : block) acc += static_cast<double>(v) * v;
    return static_cast<float>(std::sqrt(acc / static_cast<double>(block.size())));
}

}  // namespace

Recorder::Recorder(AudioSource source, int samplerate,
                   std::optional<AudioSource> mic_source, float system_gain,
                   float mic_gain)
    : samplerate_(samplerate), system_gain_(system_gain), mic_gain_(mic_gain),
      capture_(std::make_unique<AudioCapture>(std::move(source), samplerate)) {
    if (mic_source.has_value()) {
        mic_capture_ = std::make_unique<AudioCapture>(*mic_source, samplerate);
    }
}

Recorder::~Recorder() {
    stop_flag_.store(true);
    if (drain_.joinable()) drain_.join();
}

void Recorder::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.clear();
    }
    carry_[0].clear();
    carry_[1].clear();
    stop_flag_.store(false);
    paused_.store(false);
    paused_total_.store(0.0);
    level_.store(0.0f);
    started_at_ = Clock::now();

    capture_->start();   // throws: the caller reports "no usable source"
    if (mic_capture_) {
        try {
            mic_capture_->start();
        } catch (const std::exception&) {
            // A missing mic must not abort a system-audio recording.
            mic_capture_.reset();
        }
    }
    drain_ = std::thread(&Recorder::run, this);
}

void Recorder::run() {
    if (mic_capture_) {
        run_mixed();
    } else {
        run_single();
    }
}

void Recorder::run_single() {
    while (!stop_flag_.load()) {
        std::vector<float> block = capture_->drain();
        if (block.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (paused_.load()) {
            // Keep draining so the queue can't overflow, but drop the audio.
            level_.store(0.0f);
            continue;
        }
        level_.store(rms(block));
        append(block);
    }
}

void Recorder::run_mixed() {
    while (!stop_flag_.load()) {
        const bool got = pump_carry();
        std::vector<float> mixed;
        if (!take_mixed(&mixed)) {
            if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (paused_.load()) {
            level_.store(0.0f);
            continue;
        }
        level_.store(rms(mixed));
        append(mixed);
    }
}

bool Recorder::pump_carry() {
    bool got = false;
    AudioCapture* caps[2] = {capture_.get(), mic_capture_.get()};
    for (int i = 0; i < 2; ++i) {
        if (!caps[i]) continue;
        std::vector<float> block = caps[i]->drain();
        if (block.empty()) continue;
        carry_[i].insert(carry_[i].end(), block.begin(), block.end());
        got = true;
    }
    return got;
}

bool Recorder::take_mixed(std::vector<float>* out) {
    const bool mic_dead = mic_capture_ && !mic_capture_->error().empty();

    if (mic_dead) {
        // Mic failed mid-session: emit the system audio we have and drop the
        // stale mic backlog so it can't grow unbounded.
        if (carry_[0].empty()) return false;
        out->assign(carry_[0].begin(), carry_[0].end());
        for (float& v : *out) v *= system_gain_;
        carry_[0].clear();
        carry_[1].clear();
        return true;
    }

    const std::size_t n = std::min(carry_[0].size(), carry_[1].size());
    if (n == 0) return false;

    out->resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        (*out)[i] = carry_[0][i] * system_gain_ + carry_[1][i] * mic_gain_;
    }
    limit(out);
    carry_[0].erase(carry_[0].begin(), carry_[0].begin() + static_cast<long>(n));
    carry_[1].erase(carry_[1].begin(), carry_[1].begin() + static_cast<long>(n));
    return true;
}

void Recorder::append(const std::vector<float>& block) {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.insert(samples_.end(), block.begin(), block.end());
}

void Recorder::pause() {
    if (paused_.load() || stop_flag_.load()) return;
    paused_at_ = Clock::now();
    paused_.store(true);
    level_.store(0.0f);
}

void Recorder::resume() {
    if (!paused_.load()) return;
    paused_total_.store(paused_total_.load() + seconds_since(paused_at_));
    paused_.store(false);
}

std::vector<float> Recorder::stop() {
    const bool was_paused = paused_.load();

    stop_flag_.store(true);
    if (drain_.joinable()) drain_.join();

    capture_->stop();
    if (mic_capture_) mic_capture_->stop();

    // Flush whatever is still queued (skipped when paused — that audio is
    // deliberately dropped, matching pause semantics).
    if (!was_paused) {
        if (!mic_capture_) {
            append(capture_->drain());
        } else {
            pump_carry();
            std::vector<float> mixed;
            while (take_mixed(&mixed)) append(mixed);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return std::move(samples_);
}

double Recorder::elapsed() const {
    if (started_at_.time_since_epoch().count() == 0) return 0.0;
    const double extra = paused_.load() ? seconds_since(paused_at_) : 0.0;
    return std::max(0.0, seconds_since(started_at_) - paused_total_.load() - extra);
}

std::string Recorder::error() const { return capture_->error(); }

}  // namespace transcriptor::audio
