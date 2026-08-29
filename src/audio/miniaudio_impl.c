// The single translation unit that compiles miniaudio itself.
//
// Backends are trimmed to what this app needs: system-output loopback and
// microphone capture, plus a decoder for the "process an existing file" path.

#define MINIAUDIO_IMPLEMENTATION

// No playback-only machinery, no networking, no runtime linking surprises.
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_GENERATION

#ifdef _WIN32
#  define MA_NO_WINMM
#  define MA_NO_DSOUND
#  define MA_NO_JACK
#endif

#ifdef __linux__
#  define MA_NO_JACK
#  define MA_NO_OSS
#endif

#include <miniaudio.h>
