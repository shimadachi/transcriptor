// The macOS half of system_tap.h. Objective-C++ because the tap is described
// by an Objective-C object (CATapDescription); everything else here is the C
// side of Core Audio.
//
// How it fits together:
//   1. A *tap* is created from a description: a stereo mixdown of every
//      process on the machine. It is an audio object, not a device.
//   2. A tap can only be read through a device, so a private *aggregate
//      device* is created with no sub-devices and the tap attached to it.
//      Leaving the real output device out of the aggregate is deliberate: the
//      capture then does not care which device is playing, and carries on when
//      the user moves from the speakers to a headset in the middle of a call.
//   3. An I/O block on that aggregate receives the audio.
// Teardown is the same list backwards.

#include "audio/system_tap.h"

#ifdef TRANSCRIPTOR_HAVE_SYSTEM_TAP

#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <stdexcept>
#include <vector>

#include "audio/tap_convert.h"
#include "util/lang.h"

namespace transcriptor::audio {

namespace {

AudioObjectPropertyAddress global(AudioObjectPropertySelector selector) {
    return {selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
}

// Core Audio errors are four-character codes when they are printable ('who?',
// '!obj'), and those are what a search turns up, so show them as such.
std::string status_text(OSStatus status) {
    const char code[5] = {static_cast<char>(status >> 24), static_cast<char>(status >> 16),
                          static_cast<char>(status >> 8), static_cast<char>(status), 0};
    for (int i = 0; i < 4; ++i) {
        if (!std::isprint(static_cast<unsigned char>(code[i]))) return std::to_string(status);
    }
    return std::string("'") + code + "'";
}

[[noreturn]] void setup_failed(const char* en, const char* tr, OSStatus status) {
    throw std::runtime_error(std::string(L(en, tr)) + " (" + status_text(status) + ")");
}

// The rate the tap is delivering at right now. The I/O block is handed raw
// buffers and reads them as float32, so anything else is refused here rather
// than turned into noise there.
double tap_rate(AudioObjectID tap) API_AVAILABLE(macos(14.2)) {
    AudioStreamBasicDescription format{};
    UInt32 size = sizeof(format);
    const AudioObjectPropertyAddress address = global(kAudioTapPropertyFormat);
    const OSStatus err = AudioObjectGetPropertyData(tap, &address, 0, nullptr, &size, &format);
    if (err != noErr) {
        setup_failed("The format of the system audio could not be read",
                     "Sistem sesinin biçimi okunamadı", err);
    }
    const bool float32 = format.mFormatID == kAudioFormatLinearPCM &&
                         (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0 &&
                         format.mBitsPerChannel == 32;
    if (!float32 || !(format.mSampleRate > 0)) {
        throw std::runtime_error(L("System audio arrives in a format this build cannot read.",
                                   "Sistem sesi bu sürümün okuyamadığı bir biçimde geliyor."));
    }
    return format.mSampleRate;
}

// -- the permission -----------------------------------------------------------
// TCC is the framework behind every privacy prompt. These two functions are
// what the system's own frameworks call; neither is declared anywhere public,
// so they are looked up by name at run time and every step is allowed to fail.

using TccPreflight = int (*)(CFStringRef service, CFDictionaryRef options);
using TccRequest   = void (*)(CFStringRef service, CFDictionaryRef options,
                              void (^answer)(unsigned char granted));

void* tcc_symbol(const char* name) {
    static void* const handle =
        dlopen("/System/Library/PrivateFrameworks/TCC.framework/Versions/A/TCC",
               RTLD_NOW | RTLD_LOCAL);
    return handle ? dlsym(handle, name) : nullptr;
}

const CFStringRef kTccAudioCapture = CFSTR("kTCCServiceAudioCapture");

}  // namespace

TapPermission system_tap_permission() {
    if (!system_tap_available()) return TapPermission::Unknown;
    const auto preflight = reinterpret_cast<TccPreflight>(tcc_symbol("TCCAccessPreflight"));
    if (!preflight) return TapPermission::Unknown;
    switch (preflight(kTccAudioCapture, nullptr)) {
        case 0:  return TapPermission::Granted;
        case 1:  return TapPermission::Denied;
        case 2:  return TapPermission::NotAsked;
        default: return TapPermission::Unknown;
    }
}

TapPermission system_tap_request_permission(double timeout_seconds) {
    const TapPermission now = system_tap_permission();
    if (now != TapPermission::NotAsked) return now;

    // Asking on behalf of a program whose Info.plist does not say why gets
    // that program killed, not refused. The application's bundle carries the
    // text; a bare executable -- a test, a build run from its build directory
    // -- does not, and is left to the prompt the tap raises on its own.
    @autoreleasepool {
        if (![[NSBundle mainBundle]
                objectForInfoDictionaryKey:@"NSAudioCaptureUsageDescription"]) {
            return TapPermission::NotAsked;
        }
    }
    const auto request = reinterpret_cast<TccRequest>(tcc_symbol("TCCAccessRequest"));
    if (!request) return TapPermission::NotAsked;

    // Both are owned by the block as well, so an answer that comes after this
    // function has given up waiting still has somewhere to land.
    dispatch_semaphore_t answered = dispatch_semaphore_create(0);
    const auto granted = std::make_shared<std::atomic<bool>>(false);
    request(kTccAudioCapture, nullptr, ^(unsigned char yes) {
        granted->store(yes != 0);
        dispatch_semaphore_signal(answered);
    });

    const dispatch_time_t deadline = dispatch_time(
        DISPATCH_TIME_NOW, static_cast<int64_t>(timeout_seconds * NSEC_PER_SEC));
    if (dispatch_semaphore_wait(answered, deadline) != 0) return TapPermission::Pending;
    return granted->load() ? TapPermission::Granted : TapPermission::Denied;
}

struct SystemTap::State {
    AudioObjectID       tap    = kAudioObjectUnknown;
    AudioObjectID       device = kAudioObjectUnknown;
    AudioDeviceIOProcID proc   = nullptr;
    // Serial, and everything that touches the converter runs on it: the I/O
    // block and the format listener can then never overlap.
    dispatch_queue_t                 queue     = nil;
    AudioObjectPropertyListenerBlock on_format = nil;
    AudioObjectPropertyListenerBlock on_alive  = nil;

    std::atomic<bool> live{false};
    std::atomic<bool> failed{false};

    std::unique_ptr<TapConverter> converter;
    std::vector<float>            scratch;
    Sink                          sink;
    Fail                          fail;

    void deliver(const AudioBufferList* input) {
        if (!input || !converter) return;
        TapConverter::Buffer buffers[16];
        std::size_t count  = 0;
        std::size_t frames = 0;
        for (UInt32 i = 0; i < input->mNumberBuffers && count < 16; ++i) {
            const AudioBuffer& b = input->mBuffers[i];
            if (!b.mData || b.mNumberChannels == 0) continue;
            const std::size_t n = b.mDataByteSize / (sizeof(float) * b.mNumberChannels);
            frames = count == 0 ? n : std::min(frames, n);
            buffers[count].data     = static_cast<const float*>(b.mData);
            buffers[count].channels = b.mNumberChannels;
            ++count;
        }
        if (count == 0 || frames == 0) return;

        scratch.clear();
        converter->process(buffers, count, frames, &scratch);
        if (!scratch.empty() && sink) sink(scratch.data(), scratch.size());
    }

    void report(const std::string& message) {
        if (!failed.exchange(true) && fail) fail(message);
    }

    static void setup(const std::shared_ptr<State>& st, int samplerate)
        API_AVAILABLE(macos(14.2)) {
        @autoreleasepool {
            // Nothing is excluded, this app included: it plays nothing while it
            // records, and a recording being played back in the window is
            // sound the user is hearing like any other.
            CATapDescription* description =
                [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
            description.name         = @"Transcriptor";
            description.UUID         = [NSUUID UUID];
            // Private: no other process sees the tap or can read from it.
            description.privateTap   = YES;
            // The user keeps hearing what is being recorded.
            description.muteBehavior = CATapUnmuted;

            OSStatus err = AudioHardwareCreateProcessTap(description, &st->tap);
            if (err != noErr || st->tap == kAudioObjectUnknown) {
                st->tap = kAudioObjectUnknown;
                setup_failed("System audio capture could not be set up",
                             "Sistem sesi kaydı kurulamadı", err);
            }

            st->converter = std::make_unique<TapConverter>(tap_rate(st->tap), samplerate);

            NSDictionary* aggregate = @{
                @(kAudioAggregateDeviceNameKey) : @"Transcriptor system audio",
                @(kAudioAggregateDeviceUIDKey) : [[NSUUID UUID] UUIDString],
                @(kAudioAggregateDeviceSubDeviceListKey) : @[],
                @(kAudioAggregateDeviceMainSubDeviceKey) : @0,
                // Private as well, or the aggregate shows up as an input device
                // in every other app's menu for as long as a take lasts.
                @(kAudioAggregateDeviceIsPrivateKey) : @YES,
                @(kAudioAggregateDeviceIsStackedKey) : @NO,
            };
            err = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate,
                                                     &st->device);
            if (err != noErr || st->device == kAudioObjectUnknown) {
                st->device = kAudioObjectUnknown;
                setup_failed("The system audio device could not be created",
                             "Sistem sesi aygıtı oluşturulamadı", err);
            }

            NSArray*   tap_uids = @[ description.UUID.UUIDString ];
            CFArrayRef tap_list = (__bridge CFArrayRef)tap_uids;
            const AudioObjectPropertyAddress tap_list_address =
                global(kAudioAggregateDevicePropertyTapList);
            err = AudioObjectSetPropertyData(st->device, &tap_list_address, 0, nullptr,
                                             sizeof(tap_list), &tap_list);
            if (err != noErr) {
                setup_failed("System audio could not be attached to its device",
                             "Sistem sesi aygıtına bağlanamadı", err);
            }

            st->queue = dispatch_queue_create("dev.transcriptor.system-tap",
                                              DISPATCH_QUEUE_SERIAL);

            // The I/O block owns a reference, so the state outlives any cycle
            // Core Audio still has queued. The listeners below hold a weak one
            // instead: the state keeps them, and a strong one would be a cycle.
            const std::shared_ptr<State> held = st;
            err = AudioDeviceCreateIOProcIDWithBlock(
                &st->proc, st->device, st->queue,
                ^(const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp*,
                  AudioBufferList*, const AudioTimeStamp*) {
                    if (held->live.load()) held->deliver(input);
                });
            if (err != noErr || !st->proc) {
                st->proc = nullptr;
                setup_failed("System audio could not be read", "Sistem sesi okunamadı", err);
            }

            const std::weak_ptr<State> weak = st;

            // The tap follows the output device's rate. See set_in_rate().
            AudioObjectPropertyListenerBlock on_format =
                ^(UInt32, const AudioObjectPropertyAddress*) {
                    const std::shared_ptr<State> s = weak.lock();
                    if (!s || !s->live.load()) return;
                    try {
                        s->converter->set_in_rate(tap_rate(s->tap));
                    } catch (const std::exception& e) {
                        s->report(e.what());
                    }
                };
            const AudioObjectPropertyAddress format_address = global(kAudioTapPropertyFormat);
            if (AudioObjectAddPropertyListenerBlock(st->tap, &format_address, st->queue,
                                                    on_format) == noErr) {
                st->on_format = on_format;
            }

            AudioObjectPropertyListenerBlock on_alive =
                ^(UInt32, const AudioObjectPropertyAddress*) {
                    const std::shared_ptr<State> s = weak.lock();
                    if (!s || !s->live.load()) return;
                    UInt32 alive = 1;
                    UInt32 size  = sizeof(alive);
                    const AudioObjectPropertyAddress address =
                        global(kAudioDevicePropertyDeviceIsAlive);
                    if (AudioObjectGetPropertyData(s->device, &address, 0, nullptr, &size,
                                                   &alive) != noErr || alive == 0) {
                        s->report(L("System audio capture stopped unexpectedly.",
                                    "Sistem sesi kaydı beklenmedik şekilde durdu."));
                    }
                };
            const AudioObjectPropertyAddress alive_address =
                global(kAudioDevicePropertyDeviceIsAlive);
            if (AudioObjectAddPropertyListenerBlock(st->device, &alive_address, st->queue,
                                                    on_alive) == noErr) {
                st->on_alive = on_alive;
            }

            st->live.store(true);
            err = AudioDeviceStart(st->device, st->proc);
            if (err != noErr) {
                setup_failed("System audio capture could not be started",
                             "Sistem sesi kaydı başlatılamadı", err);
            }
        }
    }

    // Safe on a half-built state, which is what a failed setup() leaves.
    void teardown() API_AVAILABLE(macos(14.2)) {
        live.store(false);

        if (on_format && tap != kAudioObjectUnknown) {
            const AudioObjectPropertyAddress address = global(kAudioTapPropertyFormat);
            AudioObjectRemovePropertyListenerBlock(tap, &address, queue, on_format);
        }
        if (on_alive && device != kAudioObjectUnknown) {
            const AudioObjectPropertyAddress address =
                global(kAudioDevicePropertyDeviceIsAlive);
            AudioObjectRemovePropertyListenerBlock(device, &address, queue, on_alive);
        }
        on_format = nil;
        on_alive  = nil;

        if (device != kAudioObjectUnknown && proc) {
            AudioDeviceStop(device, proc);
            AudioDeviceDestroyIOProcID(device, proc);
            proc = nullptr;
        }

        // A block that was already queued when the device stopped still runs.
        // Wait it out before the objects it reads are destroyed, and before
        // stop() promises its caller that the sink will not be called again.
        if (queue) dispatch_sync(queue, ^{});

        if (device != kAudioObjectUnknown) {
            AudioHardwareDestroyAggregateDevice(device);
            device = kAudioObjectUnknown;
        }
        if (tap != kAudioObjectUnknown) {
            AudioHardwareDestroyProcessTap(tap);
            tap = kAudioObjectUnknown;
        }
    }
};

SystemTap::SystemTap() = default;

SystemTap::~SystemTap() { stop(); }

void SystemTap::start(int samplerate, Sink sink, Fail fail) {
    stop();

    if (@available(macOS 14.2, *)) {
        // Asked here, before anything is recorded, rather than left to the
        // prompt the tap raises by itself: that one appears over a take that
        // is already running and already silent. Half a minute is long enough
        // to read a dialog and short enough to hold a Start request for.
        permission_ = system_tap_request_permission(30.0);
        if (permission_ == TapPermission::Denied) {
            throw std::runtime_error(
                L("macOS is not allowing Transcriptor to record system audio. Turn it on "
                  "in System Settings → Privacy & Security → Screen & System Audio "
                  "Recording → System Audio Recording Only, then start again.",
                  "macOS, Transcriptor'ın sistem sesini kaydetmesine izin vermiyor. Sistem "
                  "Ayarları → Gizlilik ve Güvenlik → Ekran ve Sistem Sesi Kaydı → Yalnızca "
                  "Sistem Sesi Kaydı bölümünden açın, sonra yeniden başlatın."));
        }
        if (permission_ == TapPermission::Pending) {
            throw std::runtime_error(
                L("macOS is asking whether Transcriptor may record system audio. Answer "
                  "the prompt, then start again.",
                  "macOS, Transcriptor'ın sistem sesini kaydedip kaydedemeyeceğini soruyor. "
                  "İstemi yanıtlayın, sonra yeniden başlatın."));
        }

        auto st  = std::make_shared<State>();
        st->sink = std::move(sink);
        st->fail = std::move(fail);
        try {
            State::setup(st, samplerate);
        } catch (...) {
            st->teardown();
            throw;
        }
        state_ = st;
        return;
    }
    throw std::runtime_error(L("Recording system audio needs macOS 14.2 or newer.",
                               "Sistem sesini kaydetmek için macOS 14.2 veya üstü gerekir."));
}

void SystemTap::stop() {
    const std::shared_ptr<State> st = std::move(state_);
    state_.reset();
    if (!st) return;
    if (@available(macOS 14.2, *)) st->teardown();
}

}  // namespace transcriptor::audio

#endif  // TRANSCRIPTOR_HAVE_SYSTEM_TAP
