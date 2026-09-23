// Controls for the stand-in AudioCapture that the recording-lifecycle tests
// link in place of the real one (see fake_capture.cpp).
//
// The point of it is timing. The bugs in this area all live in the gap between
// "a device is being opened or closed" and "the app's state says so", and a
// real sound card gives no way to hold that gap open. This one does: a hook can
// park inside start() or stop() for as long as a test needs to run something
// else against the same AppState.
#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace fake_capture {

// Clears every hook, counter and queued sample. Call before each case.
void reset();

// Runs inside AudioCapture::start(), before the device counts as open, and
// inside stop(), before it returns. Either may block for as long as it likes.
void on_start(std::function<void()> hook);
void on_stop(std::function<void()> hook);

// Makes start() throw, the way a device that cannot be opened does.
void fail_start(const std::string& message);

// Samples the device will hand over. The first drain() returns all of them and
// every later one returns nothing, so a take reaches a known length promptly
// instead of depending on how long the test happens to sleep.
void set_total_samples(std::size_t n);

// The same, for one source only; it wins over set_total_samples() for that id.
// Zero is a system loopback with nothing playing: WASAPI delivers no frames at
// all then, rather than frames of silence.
void set_source_samples(const std::string& source_id, std::size_t n);

// Reports a device failure, as an unplugged headset does. The audio captured
// before it is still handed back.
void set_error(const std::string& message);

int  starts();        // devices opened
int  stops();         // devices closed
bool any_running();   // a device still open is a leaked one

}  // namespace fake_capture
