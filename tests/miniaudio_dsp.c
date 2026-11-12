/* miniaudio's signal processing and nothing else, for the tests that need its
 * resampler. The application's own copy (src/audio/miniaudio_impl.c) brings the
 * device backends with it, and with them the platform's audio libraries; a
 * test that only converts samples should not need a sound system to link. */

#define MINIAUDIO_IMPLEMENTATION

#define MA_NO_DEVICE_IO
#define MA_NO_THREADING
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_GENERATION

#include <miniaudio.h>
