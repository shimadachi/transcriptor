// Enumerate what can be recorded: system output (loopback) and microphones.
//
// Per-platform enumeration:
//   * Windows (WASAPI) — every playback device is offered as a loopback source.
//   * Linux (PulseAudio/PipeWire) — "monitor" capture devices are the loopbacks.
//   * macOS (CoreAudio) — there is no loopback device. From macOS 14.2 the
//     system mix is offered through a process tap (see system_tap.h); before
//     that only a virtual device such as BlackHole can stand in, and it is
//     detected and marked as a loopback. See macos_loopback_hint().
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace transcriptor::audio {

struct AudioSource {
    std::string id;                  // opaque, stable across re-enumeration
    std::string name;
    bool        is_loopback = false; // true = system/app output, false = real mic
    int         channels    = 2;
    bool        is_default  = false;

    // The UI draws its own icon from `is_loopback`, so the label is the name.
    const std::string& label() const { return name; }
};

// Loopback sources first, then microphones. Throws std::runtime_error if no
// audio backend could be initialised at all.
std::vector<AudioSource> list_sources();

// The monitor of the default output device — the natural "record system audio".
std::optional<AudioSource> default_loopback_source();

std::optional<AudioSource> find_source(const std::string& id);

// Non-empty on a macOS too old for the system-audio tap, when no loopback
// device exists either: a short instruction the UI can show instead of silently
// recording nothing.
std::string macos_loopback_hint();

}  // namespace transcriptor::audio
