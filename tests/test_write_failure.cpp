// Regression test for report finding R2: a failed write reported as a success.
//
// Both helpers used to `return out.good()` with the data still sitting in the
// stream buffer. The write then failed in the destructor, where nobody was
// listening -- so the app said "Saved →", the phase read Ready, and the file
// was zero bytes long. That is a lost recording reported as a kept one.
//
// RLIMIT_FSIZE=0 makes every write fail while leaving the filesystem alone.

#include "util/export.h"
#include "util/paths.h"

#include <csignal>
#include <string>
#include <vector>

#include <sys/resource.h>

#include "check.h"

using namespace transcriptor;

namespace {

// Runs `body` with a zero file-size limit, restoring the old limit afterwards
// so the test's own output can still be written.
template <typename Fn>
void with_write_failures(Fn&& body) {
    struct rlimit saved {};
    getrlimit(RLIMIT_FSIZE, &saved);
    struct rlimit none { 0, saved.rlim_max };
    setrlimit(RLIMIT_FSIZE, &none);
    body();
    setrlimit(RLIMIT_FSIZE, &saved);
}

std::uintmax_t size_of(const paths::fs::path& p) {
    std::error_code ec;
    return paths::fs::exists(p, ec) ? paths::fs::file_size(p, ec) : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_write_failure <scratch-dir>\n");
        return 2;
    }
    // Exceeding RLIMIT_FSIZE raises SIGXFSZ; we want the error, not the death.
    std::signal(SIGXFSZ, SIG_IGN);

    const paths::fs::path dir = paths::from_utf8(argv[1]);
    std::error_code ec;
    paths::fs::create_directories(dir, ec);

    const paths::fs::path txt = dir / "transcript.txt";
    const paths::fs::path wav = dir / "audio.wav";
    const std::vector<float> audio(8000, 0.1f);   // half a second at 16 kHz

    bool text_ok = true, wav_ok = true;
    with_write_failures([&] {
        text_ok = paths::write_file(txt, "hello transcript\n");
        wav_ok  = exporter::save_audio_wav(wav, audio, 16000);
    });

    test::check("write_file reports a failed write", !text_ok,
                std::to_string(size_of(txt)) + " bytes on disk");
    test::check("save_audio_wav reports a failed write", !wav_ok,
                std::to_string(size_of(wav)) + " bytes on disk");

    // ...and the ordinary path still works, which is the half of this that a
    // careless "return false" fix would break.
    const paths::fs::path txt2 = dir / "ok.txt";
    const paths::fs::path wav2 = dir / "ok.wav";
    test::check("write_file still succeeds normally",
                paths::write_file(txt2, "hello transcript\n") &&
                    size_of(txt2) == 17);
    test::check("save_audio_wav still succeeds normally",
                exporter::save_audio_wav(wav2, audio, 16000) &&
                    size_of(wav2) == 44 + audio.size() * 2);

    return test::summary("write failure");
}
