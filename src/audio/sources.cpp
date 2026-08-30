#include "audio/sources.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>

#include "audio/device_registry.h"
#include "util/lang.h"

namespace transcriptor::audio {

namespace {

struct Entry {
    ma_device_id   raw{};
    ma_device_type type = ma_device_type_capture;
    ma_uint32      channels = 2;
    bool           is_loopback = false;
    bool           is_default = false;
    std::string    name;
};

std::mutex g_mutex;
std::map<std::string, Entry> g_registry;   // guarded by g_mutex

ma_context* g_context = nullptr;
bool        g_context_failed = false;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Names that mean "this capture device is really the system output".
bool looks_like_monitor(const std::string& name) {
    const std::string n = lower(name);
    static const char* kHints[] = {"monitor", "loopback", "blackhole",
                                   "soundflower", "stereo mix", "what u hear",
                                   "wave out mix", "vb-audio", "cable output"};
    for (const char* h : kHints) {
        if (n.find(h) != std::string::npos) return true;
    }
    return false;
}

// Unique, human-stable id: "loop:Speakers" / "mic:Blue Yeti". A duplicate name
// gets a numeric suffix so both devices stay addressable.
std::string make_id(const std::string& prefix, const std::string& name,
                    const std::map<std::string, Entry>& taken) {
    std::string base = prefix + ":" + name;
    if (taken.find(base) == taken.end()) return base;
    for (int i = 2; i < 64; ++i) {
        std::string alt = base + ":" + std::to_string(i);
        if (taken.find(alt) == taken.end()) return alt;
    }
    return base;
}

// Enumerate playback + capture devices and rebuild g_registry.
// Caller must hold g_mutex.
std::vector<AudioSource> enumerate_locked() {
    ma_context* ctx = shared_context();

    ma_device_info* playback = nullptr;
    ma_uint32 playback_count = 0;
    ma_device_info* capture = nullptr;
    ma_uint32 capture_count = 0;

    if (ma_context_get_devices(ctx, &playback, &playback_count, &capture,
                               &capture_count) != MA_SUCCESS) {
        throw std::runtime_error(L("The audio devices could not be listed.",
                                   "Ses cihazları listelenemedi."));
    }

    std::map<std::string, Entry> registry;
    std::vector<AudioSource> loopbacks;
    std::vector<AudioSource> mics;

#if defined(_WIN32)
    // WASAPI can loop back any playback device, so offer all of them.
    for (ma_uint32 i = 0; i < playback_count; ++i) {
        Entry e;
        e.raw         = playback[i].id;
        e.type        = ma_device_type_loopback;
        e.channels    = 2;
        e.is_loopback = true;
        e.is_default  = playback[i].isDefault != 0;
        e.name        = playback[i].name;

        std::string id = make_id("loop", e.name, registry);
        registry[id] = e;
        loopbacks.push_back({id, e.name, true, 2, e.is_default});
    }
#else
    (void)playback;
    (void)playback_count;
#endif

    for (ma_uint32 i = 0; i < capture_count; ++i) {
        Entry e;
        e.raw        = capture[i].id;
        e.type       = ma_device_type_capture;
        e.channels   = 2;
        e.is_default = capture[i].isDefault != 0;
        e.name       = capture[i].name;
#if defined(_WIN32)
        // Playback devices already covered loopback on Windows; a capture device
        // named "Stereo Mix" is still a legitimate loopback though.
        e.is_loopback = looks_like_monitor(e.name);
#else
        e.is_loopback = looks_like_monitor(e.name);
#endif
        std::string id = make_id(e.is_loopback ? "loop" : "mic", e.name, registry);
        registry[id] = e;

        AudioSource src{id, e.name, e.is_loopback, 2, e.is_default};
        (e.is_loopback ? loopbacks : mics).push_back(src);
    }

    g_registry = std::move(registry);

    // Loopback sources first, then microphones.
    std::vector<AudioSource> out;
    out.reserve(loopbacks.size() + mics.size());
    out.insert(out.end(), loopbacks.begin(), loopbacks.end());
    out.insert(out.end(), mics.begin(), mics.end());
    return out;
}

}  // namespace

ma_context* shared_context() {
    static std::mutex ctx_mutex;
    std::lock_guard<std::mutex> lock(ctx_mutex);

    if (g_context) return g_context;
    if (g_context_failed) {
        throw std::runtime_error(L("The audio backend could not be started.",
                                   "Ses altyapısı başlatılamadı."));
    }

    auto* ctx = new ma_context();
    ma_context_config cfg = ma_context_config_init();
    if (ma_context_init(nullptr, 0, &cfg, ctx) != MA_SUCCESS) {
        delete ctx;
        g_context_failed = true;
        throw std::runtime_error(
            L("The audio backend could not be started. Is PipeWire/PulseAudio "
              "running on Linux?",
              "Ses altyapısı başlatılamadı. Linux'ta PipeWire/PulseAudio "
              "çalışıyor mu?"));
    }
    g_context = ctx;
    return g_context;
}

std::vector<AudioSource> list_sources() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return enumerate_locked();
}

std::optional<AudioSource> default_loopback_source() {
    std::vector<AudioSource> srcs;
    try {
        srcs = list_sources();
    } catch (const std::exception&) {
        return std::nullopt;
    }
    for (const auto& s : srcs) {
        if (s.is_loopback && s.is_default) return s;
    }
    for (const auto& s : srcs) {
        if (s.is_loopback) return s;
    }
    return std::nullopt;
}

std::optional<AudioSource> find_source(const std::string& id) {
    if (id.empty()) return std::nullopt;
    std::vector<AudioSource> srcs;
    try {
        srcs = list_sources();
    } catch (const std::exception&) {
        return std::nullopt;
    }
    for (const auto& s : srcs) {
        if (s.id == id) return s;
    }
    return std::nullopt;
}

std::string macos_loopback_hint() {
#ifdef __APPLE__
    try {
        for (const auto& s : list_sources()) {
            if (s.is_loopback) return {};
        }
    } catch (const std::exception&) {
        return {};
    }
    return L("macOS cannot record system audio directly. Install a virtual "
             "output device (e.g. BlackHole: `brew install blackhole-2ch`), "
             "route the sound into it and pick it as the source — or use the "
             "\"🎙 Browser\" entries in the source menu.",
             "macOS sistem sesini doğrudan kaydedemez. Sanal bir çıkış aygıtı "
             "kurun (ör. BlackHole: `brew install blackhole-2ch`), sesi ona "
             "yönlendirin ve kaynak olarak seçin — ya da kaynak menüsünden "
             "\"🎙 Tarayıcı\" seçeneklerini kullanın.");
#else
    return {};
#endif
}

bool resolve_device(const std::string& source_id, ResolvedDevice* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        enumerate_locked();   // refresh: devices come and go
    } catch (const std::exception&) {
        return false;
    }
    auto it = g_registry.find(source_id);
    if (it == g_registry.end()) return false;

    out->id       = it->second.raw;
    out->type     = it->second.type;
    out->channels = it->second.channels;
    out->name     = it->second.name;
    out->use_default_id = false;
    return true;
}

}  // namespace transcriptor::audio
