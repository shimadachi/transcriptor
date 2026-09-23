#include "audio/decode.h"

#include <cstring>
#include <stdexcept>

#include <miniaudio.h>

#include "util/lang.h"
#include "util/net.h"
#include "util/utf8.h"

namespace transcriptor::audio {

namespace {

[[noreturn]] void throw_cancelled() {
    throw std::runtime_error(L("Decoding was cancelled.", "Çözme iptal edildi."));
}

// Read the whole stream in chunks; ma_decoder already converts format,
// channel count and sample rate to what we asked for.
std::vector<float> decode_with_miniaudio(const paths::fs::path& path,
                                         int samplerate, bool* handled,
                                         net::Canceller* cancel) {
    *handled = false;

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1,
                                                   static_cast<ma_uint32>(samplerate));
    ma_decoder decoder;
#ifdef _WIN32
    ma_result rc = ma_decoder_init_file_w(path.c_str(), &cfg, &decoder);
#else
    ma_result rc = ma_decoder_init_file(path.string().c_str(), &cfg, &decoder);
#endif
    if (rc != MA_SUCCESS) return {};   // unsupported container -> try ffmpeg

    *handled = true;

    std::vector<float> out;
    // Pre-size when the length is known, so long files don't rehash the buffer.
    ma_uint64 total_frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames) == MA_SUCCESS &&
        total_frames > 0) {
        out.reserve(static_cast<std::size_t>(total_frames));
    }

    constexpr std::size_t kChunk = 16384;
    std::vector<float> buf(kChunk);
    for (;;) {
        // Between chunks, so a cancelled hour-long file stops in milliseconds
        // rather than at the end of the decode. The decoder is closed on the
        // way out; the exception carries the reason.
        if (cancel && cancel->requested()) {
            ma_decoder_uninit(&decoder);
            throw_cancelled();
        }
        ma_uint64 read = 0;
        rc = ma_decoder_read_pcm_frames(&decoder, buf.data(), kChunk, &read);
        if (read > 0) {
            out.insert(out.end(), buf.begin(),
                       buf.begin() + static_cast<long>(read));
        }
        if (rc != MA_SUCCESS || read < kChunk) break;
    }
    ma_decoder_uninit(&decoder);
    return out;
}

std::vector<float> decode_with_ffmpeg(const paths::fs::path& path, int samplerate,
                                      net::Canceller* cancel) {
    // ffmpeg writes raw f32le to a temp file; reading a pipe would mean
    // reimplementing the streaming reader for a rare path.
    const paths::fs::path raw = path.string() + ".f32";

    // run() kills the child on a cancel, which is the only way to interrupt it:
    // this thread is blocked in the wait until ffmpeg is done with the file.
    net::ProcResult r = net::run({
        "ffmpeg", "-nostdin", "-y", "-loglevel", "error",
        "-i", paths::to_utf8(path),
        "-f", "f32le", "-ac", "1", "-ar", std::to_string(samplerate),
        paths::to_utf8(raw)}, 0, cancel);

    std::error_code ec;
    struct Cleanup {
        const paths::fs::path& p;
        ~Cleanup() { std::error_code e; paths::fs::remove(p, e); }
    } cleanup{raw};

    if (r.cancelled) throw_cancelled();
    if (!r.launched) {
        throw std::runtime_error(
            L("This audio format could not be decoded and ffmpeg was not found. "
              "Try WAV/MP3/FLAC, or install ffmpeg.",
              "Bu ses biçimi çözülemedi ve ffmpeg bulunamadı. WAV/MP3/FLAC "
              "deneyin ya da ffmpeg kurun."));
    }
    if (r.exit_code != 0) {
        const std::string tail = utf8::tail(r.output, 400);
        throw std::runtime_error(L("ffmpeg decoding error: ",
                                   "ffmpeg çözme hatası: ") + tail);
    }

    std::string bytes;
    if (!paths::read_file(raw, &bytes) || bytes.empty()) {
        throw std::runtime_error(L("The audio decoded, but no data came back.",
                                   "Ses çözüldü ama veri okunamadı."));
    }

    std::vector<float> out(bytes.size() / sizeof(float));
    std::memcpy(out.data(), bytes.data(), out.size() * sizeof(float));
    return out;
}

}  // namespace

std::vector<float> decode_file(const paths::fs::path& path, int samplerate,
                               net::Canceller* cancel) {
    std::error_code ec;
    if (!paths::fs::exists(path, ec)) {
        throw std::runtime_error(L("File not found: ", "Dosya bulunamadı: ") +
                                 paths::to_utf8(path));
    }
    // A cancel that arrived before this call starts still counts: the worker
    // may have been admitted seconds ago and only reached the decode now.
    if (cancel && cancel->requested()) throw_cancelled();

    bool handled = false;
    std::vector<float> out = decode_with_miniaudio(path, samplerate, &handled, cancel);
    if (handled && !out.empty()) return out;

    return decode_with_ffmpeg(path, samplerate, cancel);
}

}  // namespace transcriptor::audio
