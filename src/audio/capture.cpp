#include "audio/capture.h"

#include <atomic>
#include <deque>
#include <stdexcept>

#include "audio/device_registry.h"
#include "util/lang.h"

namespace transcriptor::audio {

namespace {
// ~30 s of 16 kHz mono. The recorder drains far more often than this; the cap
// only exists so a stalled consumer cannot grow the queue without bound.
constexpr std::size_t kMaxQueuedSamples = 16000 * 30;
}  // namespace

struct AudioCapture::Impl {
    ma_device         device{};
    bool              device_ready = false;
    std::atomic<bool> started{false};

    std::mutex         mutex;
    std::deque<float>  queue;
    std::string        error;

    void push(const float* samples, std::size_t count) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.insert(queue.end(), samples, samples + count);
        // Drop the oldest audio rather than the newest, matching the Python
        // capture queue's behaviour.
        while (queue.size() > kMaxQueuedSamples) queue.pop_front();
    }

    void set_error(std::string msg) {
        std::lock_guard<std::mutex> lock(mutex);
        if (error.empty()) error = std::move(msg);
    }

    // miniaudio hands these a void* — they live here as statics so they can see
    // Impl, which is private to AudioCapture.
    static void on_data(ma_device* device, void* /*output*/, const void* input,
                        ma_uint32 frame_count) {
        auto* impl = static_cast<Impl*>(device->pUserData);
        if (!impl || !input || frame_count == 0) return;
        impl->push(static_cast<const float*>(input), frame_count);
    }

    static void on_stop(ma_device* device) {
        auto* impl = static_cast<Impl*>(device->pUserData);
        // Only meaningful when the device stopped on its own (unplugged, sink
        // removed); an explicit stop() clears `started` first.
        if (impl && impl->started.load()) {
            impl->set_error(L("The audio device stopped unexpectedly.",
                              "Ses aygıtı beklenmedik şekilde durdu."));
        }
    }
};

AudioCapture::AudioCapture(AudioSource source, int samplerate)
    : source_(std::move(source)), samplerate_(samplerate),
      impl_(std::make_unique<Impl>()) {}

AudioCapture::~AudioCapture() { stop(); }

void AudioCapture::start() {
    if (impl_->started.load()) return;

    ResolvedDevice dev;
    if (!resolve_device(source_.id, &dev)) {
        throw std::runtime_error(L("Audio source not found: ",
                                   "Ses kaynağı bulunamadı: ") + source_.name);
    }

    ma_device_config cfg = ma_device_config_init(dev.type);
    cfg.capture.pDeviceID = dev.use_default_id ? nullptr : &dev.id;
    cfg.capture.format    = ma_format_f32;
    cfg.capture.channels  = 1;         // miniaudio downmixes for us
    cfg.capture.shareMode = ma_share_mode_shared;
    cfg.sampleRate        = static_cast<ma_uint32>(samplerate_);
    cfg.periodSizeInFrames = static_cast<ma_uint32>(samplerate_ / 4);  // ~250 ms
    cfg.dataCallback      = Impl::on_data;
    cfg.notificationCallback = nullptr;
    cfg.stopCallback      = Impl::on_stop;
    cfg.pUserData         = impl_.get();

    ma_result rc = ma_device_init(shared_context(), &cfg, &impl_->device);
    if (rc != MA_SUCCESS) {
        throw std::runtime_error(L("The audio device could not be opened (",
                                   "Ses aygıtı açılamadı (") + source_.name +
                                 "): " + ma_result_description(rc));
    }
    impl_->device_ready = true;

    rc = ma_device_start(&impl_->device);
    if (rc != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        impl_->device_ready = false;
        throw std::runtime_error(L("Recording could not be started (",
                                   "Kayıt başlatılamadı (") + source_.name +
                                 "): " + ma_result_description(rc));
    }
    impl_->started.store(true);
}

void AudioCapture::stop() {
    if (!impl_) return;
    impl_->started.store(false);
    if (impl_->device_ready) {
        ma_device_uninit(&impl_->device);   // implies stop
        impl_->device_ready = false;
    }
}

std::vector<float> AudioCapture::drain() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<float> out(impl_->queue.begin(), impl_->queue.end());
    impl_->queue.clear();
    return out;
}

bool AudioCapture::running() const { return impl_ && impl_->started.load(); }

std::string AudioCapture::error() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->error;
}

}  // namespace transcriptor::audio
