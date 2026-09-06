// Regression test for report finding R2: a failed write reported as a success.
//
// Both helpers used to `return out.good()` with the data still sitting in the
// stream buffer. The write then failed in the destructor, where nobody was
// listening -- so the app said "Saved →", the phase read Ready, and the file
// was zero bytes long. That is a lost recording reported as a kept one.
//
// Also for the finding after it: a failed write that had already destroyed the
// file it was replacing. write_file() truncated the destination before it knew
// whether it could fill it, so overwriting a summary with a full disk reported
// the failure honestly -- and left zero bytes where the old summary had been.
//
// RLIMIT_FSIZE=0 makes every write fail while leaving the filesystem alone.

#include "util/export.h"
#include "util/paths.h"

#include <csignal>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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

std::string contents(const paths::fs::path& p) {
    std::string s;
    return paths::read_file(p, &s) ? s : std::string("<missing>");
}

// Staging files are siblings of the destination; a failed write must not leave
// one lying next to the transcript for the user to find.
int stray_files(const paths::fs::path& dir, const std::string& stem) {
    std::error_code ec;
    int n = 0;
    for (const auto& e : paths::fs::directory_iterator(dir, ec)) {
        const std::string name = paths::to_utf8(e.path().filename());
        if (name != stem && name.compare(0, stem.size(), stem) == 0) ++n;
    }
    return n;
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

    // -- a failed replacement keeps what was already there --------------------
    // The summary someone spent a meeting producing, deliberately overwritten
    // by a re-run that then hits a full disk.
    const paths::fs::path summary = dir / "summary.txt";
    const std::string valuable = "the summary worth keeping\n";
    test::check("the existing summary was written",
                exporter::save_text(summary, valuable));

    bool replaced = true;
    with_write_failures([&] {
        replaced = exporter::save_text(summary, "the replacement that cannot land");
    });
    test::check("a failed replacement reports failure", !replaced);
    test::check("a failed replacement keeps the old summary",
                contents(summary) == valuable, contents(summary));
    test::check("a failed replacement leaves no staging file behind",
                stray_files(dir, "summary.txt") == 0);

    // -- the transcript pair moves together ------------------------------------
    // .txt and .json are one result in two shapes. Replacing one and failing on
    // the other left the session claiming a transcript it no longer had.
    const paths::fs::path tx_txt  = dir / "pair.txt";
    const paths::fs::path tx_json = dir / "pair.json";
    const nlohmann::json old_json = {{"text", "first pass"}};
    test::check("the existing transcript pair was written",
                exporter::save_transcript(tx_txt, "first pass\n", tx_json, old_json));

    bool pair_ok = true;
    with_write_failures([&] {
        pair_ok = exporter::save_transcript(tx_txt, "second pass\n", tx_json,
                                            nlohmann::json{{"text", "second pass"}});
    });
    test::check("a failed transcript replacement reports failure", !pair_ok);
    test::check("a failed transcript replacement keeps the old text",
                contents(tx_txt) == "first pass\n", contents(tx_txt));
    test::check("a failed transcript replacement keeps the old json",
                contents(tx_json) == old_json.dump(2), contents(tx_json));

    // ...and the ordinary replacement still lands, both halves of it.
    const nlohmann::json new_json = {{"text", "second pass"}};
    test::check("a transcript pair still replaces normally",
                exporter::save_transcript(tx_txt, "second pass\n", tx_json, new_json) &&
                    contents(tx_txt) == "second pass\n" &&
                    contents(tx_json) == new_json.dump(2));

    return test::summary("write failure");
}
