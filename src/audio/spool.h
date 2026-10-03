// Stream a take to disk while it is being captured, instead of holding all of
// it in memory.
//
// A take used to live entirely in one std::vector<float> that grew for as long
// as the recording ran. That is 230 MB an hour at 16 kHz -- survivable on its
// own -- but a vector cannot grow in place: every time it outgrew its capacity
// it allocated a larger block, copied the whole take into it, and only then
// freed the old one. A five-hour recording therefore asked the host for two
// and a half to three gigabytes at once, several times over, and took the
// machine into its pagefile: the app did not crash, the whole desktop stopped
// responding. Writing each block out as it arrives keeps the recorder's
// footprint flat and constant, however long the take runs.
//
// The spool is the session's audio.wav, so saving a recording afterwards costs
// no second pass and no second copy -- it is already written when Stop is
// pressed. It is also the only reason a take now survives a crash: everything
// up to the last block is on disk, and recover_wav() makes it playable again.
#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "util/paths.h"

namespace transcriptor::audio {

namespace fs = paths::fs;

// A 16-bit PCM mono WAV written incrementally.
//
// Not internally synchronized: the recorder calls this under the same lock
// that guards its session buffer, which is also what keeps the drain thread
// and the tail written by stop() from interleaving.
class WavSpool {
public:
    WavSpool() = default;
    ~WavSpool();

    WavSpool(const WavSpool&)            = delete;
    WavSpool& operator=(const WavSpool&) = delete;

    // Creates `path` and lays down a header whose two length fields are still
    // zero; finish() fills them in. False if the file cannot be created, with
    // error() saying why -- the caller keeps recording into memory instead,
    // because a take nobody can write is still a take worth keeping.
    bool open(const fs::path& path, int samplerate);

    bool is_open() const { return out_.is_open(); }

    // Appends one block of mono float32 in [-1, 1]. The first failure latches:
    // later calls do nothing and return false, so a disk that filled up is
    // reported once rather than once per block for the rest of the take.
    bool write(const float* data, std::size_t count);
    bool write(const std::vector<float>& block) {
        return write(block.data(), block.size());
    }

    // Patches the header's length fields, flushes and closes. Must succeed
    // before the file is read back as a WAV. Calling it twice is harmless.
    bool finish();

    // Close and delete the file -- Cancel, whose audio nobody wants. Safe to
    // call whether or not finish() already ran.
    void discard();

    std::uint64_t     frames() const { return frames_; }
    const fs::path&   path() const { return path_; }
    const std::string& error() const { return error_; }

private:
    bool fail(const std::string& message);

    fs::path      path_;
    std::ofstream out_;
    int           samplerate_ = 0;
    std::uint64_t frames_     = 0;
    bool          finished_   = false;
    std::string   error_;
};

// Read a mono 16-bit PCM WAV back as float32.
//
// The output is sized once, from the header, so reading a long take costs one
// allocation of exactly its length. That matters here for the same reason the
// spool does: growing into it a block at a time would reintroduce the copying
// spike this whole file exists to remove.
bool read_wav_f32(const fs::path& path, std::vector<float>* out,
                  std::string* error);

// Repair the length fields of a spool that was never finished, because the app
// was killed or the machine lost power mid-take. Every block that reached the
// disk is still there; only the header disagrees with the file size. Returns
// true when the file is consistent afterwards -- including when it already
// was -- and false when it is not a spool this can repair.
bool recover_wav(const fs::path& path, std::string* error);

// How many frames a spool may hold. WAV keeps its sizes in 32-bit fields, so
// the format itself runs out at four gigabytes: a little over 37 hours at
// 16 kHz. The spool stops accepting audio there rather than wrap the header
// round and call a corrupt file a recording.
std::uint64_t max_spool_frames();

}  // namespace transcriptor::audio
