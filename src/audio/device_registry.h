// Internal: maps the opaque AudioSource::id back to a concrete miniaudio
// device. Only sources.cpp and capture.cpp include this.
#pragma once

#include <string>

#include <miniaudio.h>

namespace transcriptor::audio {

struct ResolvedDevice {
    ma_device_id   id{};
    ma_device_type type = ma_device_type_capture;  // or ma_device_type_loopback
    ma_uint32      channels = 2;
    bool           use_default_id = false;         // pass NULL for the device id
    std::string    name;
};

// Refreshes the registry by enumerating, then looks `source_id` up.
// Returns false when the id no longer matches any present device.
bool resolve_device(const std::string& source_id, ResolvedDevice* out);

// Shared context, initialised once. Throws std::runtime_error on failure.
ma_context* shared_context();

}  // namespace transcriptor::audio
