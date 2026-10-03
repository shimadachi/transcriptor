#include "audio/spool.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "util/lang.h"

namespace transcriptor::audio {

namespace {

// Canonical 44-byte WAV header: RIFF/WAVE, one `fmt ` chunk, one `data` chunk.
constexpr std::streamoff kHeaderBytes = 44;
constexpr std::streamoff kRiffSizeAt  = 4;    // uint32: 36 + data bytes
constexpr std::streamoff kDataSizeAt  = 40;   // uint32: data bytes

// How much is converted to int16 at a time. Blocks arrive at about a quarter
// second (4000 samples at 16 kHz), so one pass covers a typical block and the
// buffer stays small enough to leave on the stack.
constexpr std::size_t kStageSamples = 4096;

void put_u32(std::string* s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_u16(std::string* s, std::uint16_t v) {
    for (int i = 0; i < 2; ++i) s->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

// Little-endian on every host, which is what the format says -- not whatever
// this machine's int16 happens to look like in memory.
void put_i16_le(unsigned char* at, std::int16_t v) {
    const std::uint16_t u = static_cast<std::uint16_t>(v);
    at[0] = static_cast<unsigned char>(u & 0xFF);
    at[1] = static_cast<unsigned char>((u >> 8) & 0xFF);
}

std::uint32_t get_u32_le(const unsigned char* at) {
    return static_cast<std::uint32_t>(at[0]) |
           (static_cast<std::uint32_t>(at[1]) << 8) |
           (static_cast<std::uint32_t>(at[2]) << 16) |
           (static_cast<std::uint32_t>(at[3]) << 24);
}
std::uint16_t get_u16_le(const unsigned char* at) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(at[0]) |
                                      (static_cast<std::uint16_t>(at[1]) << 8));
}

std::string header_bytes(int samplerate, std::uint32_t data_bytes) {
    std::string hdr;
    hdr.reserve(static_cast<std::size_t>(kHeaderBytes));
    hdr += "RIFF";
    put_u32(&hdr, 36 + data_bytes);
    hdr += "WAVE";
    hdr += "fmt ";
    put_u32(&hdr, 16);                                          // PCM chunk size
    put_u16(&hdr, 1);                                           // format: PCM
    put_u16(&hdr, 1);                                           // channels: mono
    put_u32(&hdr, static_cast<std::uint32_t>(samplerate));
    put_u32(&hdr, static_cast<std::uint32_t>(samplerate) * 2);  // byte rate
    put_u16(&hdr, 2);                                           // block align
    put_u16(&hdr, 16);                                          // bits per sample
    hdr += "data";
    put_u32(&hdr, data_bytes);
    return hdr;
}

// Both length fields have to fit in 32 bits, and the RIFF one carries the
// extra 36 bytes of header with it -- so that, not the data field, is the
// binding limit.
constexpr std::uint64_t kMaxDataBytes = 0xFFFFFFFFull - 36;

}  // namespace

std::uint64_t max_spool_frames() { return kMaxDataBytes / 2; }

WavSpool::~WavSpool() {
    // A spool destroyed without finish() -- an exception on the way out of a
    // take -- still has its length fields at zero. Patch them rather than
    // leave a file that says it holds nothing.
    if (out_.is_open() && !finished_) finish();
}

bool WavSpool::fail(const std::string& message) {
    if (error_.empty()) error_ = message;
    return false;
}

bool WavSpool::open(const fs::path& path, int samplerate) {
    path_       = path;
    samplerate_ = samplerate;
    frames_     = 0;
    finished_   = false;
    error_.clear();

    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);

    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) {
        return fail(L("The recording could not be opened for writing: ",
                      "Kayıt yazmak için açılamadı: ") + paths::to_utf8(path));
    }

    const std::string hdr = header_bytes(samplerate, 0);
    out_.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    if (!out_) {
        return fail(L("The recording could not be started on disk: ",
                      "Kayıt diskte başlatılamadı: ") + paths::to_utf8(path));
    }
    return true;
}

bool WavSpool::write(const float* data, std::size_t count) {
    if (!error_.empty()) return false;   // latched; see the header
    if (!out_.is_open()) return fail(L("The recording file is not open.",
                                       "Kayıt dosyası açık değil."));
    if (finished_) return fail(L("The recording file is already closed.",
                                 "Kayıt dosyası zaten kapatıldı."));
    if (count == 0) return true;

    if (frames_ + count > max_spool_frames()) {
        // Everything written so far stays valid: finish() patches the header
        // with the bytes that really landed, so the take up to here is a
        // playable WAV rather than a corrupt one.
        return fail(L("The recording reached the 4 GB limit of the WAV format.",
                      "Kayıt, WAV biçiminin 4 GB sınırına ulaştı."));
    }

    std::array<unsigned char, kStageSamples * 2> stage{};
    std::size_t done = 0;
    while (done < count) {
        const std::size_t n = std::min(kStageSamples, count - done);
        for (std::size_t i = 0; i < n; ++i) {
            const float v = std::clamp(data[done + i], -1.0f, 1.0f);
            put_i16_le(&stage[i * 2], static_cast<std::int16_t>(v * 32767.0f));
        }
        out_.write(reinterpret_cast<const char*>(stage.data()),
                   static_cast<std::streamsize>(n * 2));
        if (!out_) {
            // The usual cause is a full disk, and it is the one failure here
            // that loses audio -- so it is reported, never swallowed.
            return fail(L("The recording could not be written to ",
                          "Kayıt şuraya yazılamadı: ") + paths::to_utf8(path_));
        }
        done += n;
    }
    frames_ += count;
    return true;
}

bool WavSpool::finish() {
    if (finished_) return error_.empty();
    if (!out_.is_open()) return fail(L("The recording file is not open.",
                                       "Kayıt dosyası açık değil."));
    finished_ = true;

    const std::uint64_t data_bytes = frames_ * 2;
    const std::uint32_t data32 =
        static_cast<std::uint32_t>(std::min(data_bytes, kMaxDataBytes));

    std::string riff;
    put_u32(&riff, 36 + data32);
    std::string data;
    put_u32(&data, data32);

    out_.seekp(kRiffSizeAt, std::ios::beg);
    out_.write(riff.data(), 4);
    out_.seekp(kDataSizeAt, std::ios::beg);
    out_.write(data.data(), 4);
    out_.flush();

    const bool good = out_.good();
    // Checked before answering, for the reason paths::write_file spells out:
    // this is the only copy of the recording, and a destructor that fails
    // silently reports a lost take as a saved one.
    out_.close();
    if (!good || !out_.good()) {
        return fail(L("The recording could not be finished on disk: ",
                      "Kayıt diskte tamamlanamadı: ") + paths::to_utf8(path_));
    }
    return error_.empty();
}

void WavSpool::discard() {
    if (out_.is_open()) out_.close();
    finished_ = true;
    if (!path_.empty()) {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    frames_ = 0;
}

bool read_wav_f32(const fs::path& path, std::vector<float>* out,
                  std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (!out) return fail("read_wav_f32: no destination");
    out->clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail(L("The recording could not be read back from ",
                      "Kayıt şuradan geri okunamadı: ") + paths::to_utf8(path));
    }

    unsigned char hdr[kHeaderBytes];
    in.read(reinterpret_cast<char*>(hdr), kHeaderBytes);
    if (in.gcount() != kHeaderBytes || std::memcmp(hdr, "RIFF", 4) != 0 ||
        std::memcmp(hdr + 8, "WAVE", 4) != 0 ||
        std::memcmp(hdr + 36, "data", 4) != 0) {
        return fail(L("That file is not a recording this app wrote: ",
                      "Bu dosya uygulamanın yazdığı bir kayıt değil: ") +
                    paths::to_utf8(path));
    }
    if (get_u16_le(hdr + 20) != 1 || get_u16_le(hdr + 22) != 1 ||
        get_u16_le(hdr + 34) != 16) {
        return fail(L("That recording is not 16-bit mono PCM: ",
                      "Bu kayıt 16-bit mono PCM değil: ") + paths::to_utf8(path));
    }

    // The header is the size to trust only as far as the file goes.
    //
    // A spool the app never got to finish still says zero, because the length
    // fields are written last -- and a take must not be lost to four bytes
    // when every second of it is sitting right there. So zero with samples
    // after it means "as much as the file holds", and any other value is
    // clamped to what is really there, for a spool a crash cut short.
    std::error_code ec;
    const std::uintmax_t on_disk = fs::file_size(path, ec);
    const bool know_size = !ec && on_disk >= static_cast<std::uintmax_t>(kHeaderBytes);
    const std::uint64_t after_header =
        know_size ? static_cast<std::uint64_t>(on_disk) - kHeaderBytes : 0;
    std::uint64_t data_bytes = get_u32_le(hdr + kDataSizeAt);
    if (know_size) {
        data_bytes = (data_bytes == 0) ? after_header
                                       : std::min(data_bytes, after_header);
    }

    const std::size_t frames = static_cast<std::size_t>(data_bytes / 2);
    // One allocation, of exactly the right size. See the note in the header.
    out->resize(frames);

    std::array<unsigned char, kStageSamples * 2> stage{};
    std::size_t done = 0;
    while (done < frames) {
        const std::size_t n = std::min(kStageSamples, frames - done);
        in.read(reinterpret_cast<char*>(stage.data()),
                static_cast<std::streamsize>(n * 2));
        const std::size_t got = static_cast<std::size_t>(in.gcount()) / 2;
        for (std::size_t i = 0; i < got; ++i) {
            const std::int16_t s = static_cast<std::int16_t>(get_u16_le(&stage[i * 2]));
            (*out)[done + i] = static_cast<float>(s) / 32768.0f;
        }
        done += got;
        if (got < n) {
            // Short read: keep what is there rather than fail the whole take.
            out->resize(done);
            break;
        }
    }
    return true;
}

bool recover_wav(const fs::path& path, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };

    std::error_code ec;
    const std::uintmax_t on_disk = fs::file_size(path, ec);
    if (ec || on_disk < static_cast<std::uintmax_t>(kHeaderBytes)) {
        return fail(L("There is nothing to recover in ",
                      "Kurtarılacak bir şey yok: ") + paths::to_utf8(path));
    }

    std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!io) {
        return fail(L("The recording could not be opened for repair: ",
                      "Kayıt onarım için açılamadı: ") + paths::to_utf8(path));
    }
    unsigned char hdr[kHeaderBytes];
    io.read(reinterpret_cast<char*>(hdr), kHeaderBytes);
    if (io.gcount() != kHeaderBytes || std::memcmp(hdr, "RIFF", 4) != 0 ||
        std::memcmp(hdr + 8, "WAVE", 4) != 0 ||
        std::memcmp(hdr + 36, "data", 4) != 0) {
        return fail(L("That file is not a recording this app wrote: ",
                      "Bu dosya uygulamanın yazdığı bir kayıt değil: ") +
                    paths::to_utf8(path));
    }

    // Whole frames only: a spool cut mid-sample has half a frame at the end,
    // and half a frame is not audio.
    const std::uint64_t have = (static_cast<std::uint64_t>(on_disk) - kHeaderBytes) / 2 * 2;
    const std::uint32_t data32 =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(have, kMaxDataBytes));
    if (get_u32_le(hdr + kDataSizeAt) == data32 &&
        get_u32_le(hdr + kRiffSizeAt) == 36 + data32) {
        return true;   // already consistent; nothing to do
    }

    std::string riff;
    put_u32(&riff, 36 + data32);
    std::string data;
    put_u32(&data, data32);

    io.seekp(kRiffSizeAt, std::ios::beg);
    io.write(riff.data(), 4);
    io.seekp(kDataSizeAt, std::ios::beg);
    io.write(data.data(), 4);
    io.flush();
    const bool good = io.good();
    io.close();
    if (!good) {
        return fail(L("The recording could not be repaired: ",
                      "Kayıt onarılamadı: ") + paths::to_utf8(path));
    }
    return true;
}

}  // namespace transcriptor::audio
