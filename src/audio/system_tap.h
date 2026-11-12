// System audio on macOS without a helper driver: a Core Audio process tap.
//
// macOS has no loopback device, which is why this app used to send people off
// to install BlackHole. Since macOS 14.2 the OS can hand a process the mix of
// what every other process is playing, behind its own "System Audio Recording"
// permission -- audio only, unlike ScreenCaptureKit, which gets at the same
// sound by asking for the right to record the screen.
//
// The tap is not a device miniaudio can open, so it does not go through the
// usual capture path: system_tap_mac.mm drives it and capture.cpp picks it
// when the source is kSystemTapSourceId.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

// The tap API arrived in the macOS 14.2 SDK. An older SDK builds the app
// without it rather than failing, and the BlackHole route still works there.
#if defined(__APPLE__) && defined(__has_include)
#  if __has_include(<CoreAudio/AudioHardwareTapping.h>)
#    define TRANSCRIPTOR_HAVE_SYSTEM_TAP 1
#  endif
#endif

namespace transcriptor::audio {

// The one source id that is not a miniaudio device.
inline constexpr char kSystemTapSourceId[] = "tap:system";

// Built with the API *and* running on a macOS that has it. The deployment
// target is far older than 14.2, so having compiled against it says nothing
// about the machine the binary ends up on.
inline bool system_tap_available() {
#ifdef TRANSCRIPTOR_HAVE_SYSTEM_TAP
    if (__builtin_available(macOS 14.2, *)) return true;
#endif
    return false;
}

// Whether macOS lets this app record system audio.
//
// There is no public way to ask. A tap is created and started the same whether
// the user allowed it or refused, and a refused one delivers silence -- which
// is also what an allowed one delivers while nothing is playing. So this reads
// the answer from the system's own permission framework, through two functions
// it does not export in any header. They may be gone in some future macOS, and
// everything here is written for that day: Unknown is a normal answer, and it
// means "carry on as though this check did not exist".
enum class TapPermission {
    Unknown,    // could not be determined; do not act on it
    Granted,
    Denied,
    NotAsked,   // the user has never been shown the prompt
    Pending,    // the prompt is up and was not answered in time
};

#ifdef TRANSCRIPTOR_HAVE_SYSTEM_TAP

// What the system says right now. Never shows anything.
TapPermission system_tap_permission();

// The same, except that a user who was never asked is asked now, and this
// waits up to `timeout_seconds` for the answer.
TapPermission system_tap_request_permission(double timeout_seconds);

class SystemTap {
public:
    // Mono float32 at the rate start() was given. Called on a queue of the
    // tap's own, never after stop() has returned.
    using Sink = std::function<void(const float* samples, std::size_t count)>;
    // The capture died on its own. Called at most once, on that same queue.
    using Fail = std::function<void(const std::string& message)>;

    SystemTap();
    ~SystemTap();

    SystemTap(const SystemTap&) = delete;
    SystemTap& operator=(const SystemTap&) = delete;

    // Throws std::runtime_error when the tap cannot be set up, and when the
    // user has refused the permission or has not answered the prompt yet: a
    // take that would be silence is not started. Where the permission cannot
    // be read at all the tap starts regardless, and permission() says so.
    void start(int samplerate, Sink sink, Fail fail);
    void stop();

    // What start() found. Anything but Granted means silence from this tap
    // may be the OS withholding the audio rather than nothing playing.
    TapPermission permission() const { return permission_; }

private:
    struct State;
    std::shared_ptr<State> state_;
    TapPermission          permission_ = TapPermission::Unknown;
};

#endif  // TRANSCRIPTOR_HAVE_SYSTEM_TAP

}  // namespace transcriptor::audio
