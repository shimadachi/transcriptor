// Regression tests for names in languages ASCII cannot spell (V13, V15).
//
// V13: model downloads into a folder named in a language the system code page
// cannot spell.
//
// The download built its ".part" name through fs::path::string(), which on
// Windows converts to the ANSI code page and throws when a character has no
// place in it. The models folder lives under the user's profile, so a user
// named, say, Ağaoğlu on an English Windows got an exception -- thrown on the
// download thread, which is std::terminate: the app vanished mid-download.
//
// Meaningful on Windows, where the conversion happens; elsewhere it confirms
// the path is left as it was given.
//
// V15: an upload keeps its name in the session folder, and the name was made
// "safe" byte by byte: every byte of every non-ASCII letter became "_", so
// "Toplantı kaydı.wav" was saved as "Toplant__ kayd__.wav". A file named ".."
// could not be copied at all.

#include <string>

#include "check.h"
#include "util/net.h"
#include "util/paths.h"

using namespace transcriptor;

int main(int argc, char** argv) {
    const paths::fs::path scratch =
        paths::from_utf8(argc > 1 ? argv[1] : "unicode_paths_scratch");
    std::error_code ec;
    paths::fs::remove_all(scratch, ec);
    // ğ and ş are in neither Windows-1252 nor any Western code page.
    const paths::fs::path dir = scratch / paths::from_utf8("Ağaoğlu şirketi");
    paths::fs::create_directories(dir, ec);

    std::string thrown;
    net::DownloadResult r;
    try {
        // A scheme curl refuses at once, so nothing touches the network.
        r = net::download("unsupported://model.bin", dir / "model.bin", 0, nullptr, nullptr);
    } catch (const std::exception& e) {
        thrown = e.what();
    }
    test::check("V13 a download into a non-ANSI folder does not throw", thrown.empty(),
                thrown);
    test::check("V13 it reports its failure like any other", !r.ok && !r.error.empty(),
                r.error);
    test::check("V13 and leaves no partial file behind",
                !paths::fs::exists(dir / "model.bin.part", ec));
    // -- V15 --------------------------------------------------------------
    const auto same = [](const std::string& in, const std::string& want) {
        const std::string got = paths::safe_filename(in);
        test::check(("V15 upload name \"" + in + "\" -> \"" + want + "\"").c_str(),
                    got == want, got);
    };
    same("Toplantı kaydı.wav", "Toplantı kaydı.wav");
    same("Ağaoğlu — görüşme.m4a", "Ağaoğlu — görüşme.m4a");
    same("C:\\Users\\x\\Desktop\\toplantı.mp3", "toplantı.mp3");
    same("../../etc/passwd", "passwd");
    same("..", "audio");
    same(" .hidden.wav ", "hidden.wav");
    same("a:b?c*.wav", "a_b_c_.wav");
    same("con.wav", "_con.wav");
    same("COM3.mp3", "_COM3.mp3");
    same(std::string("bad\xC4.wav"), "bad_.wav");   // half a character
    const std::string long_name = std::string(200, 'x') + "ş.wav";
    const std::string cut = paths::safe_filename(long_name);
    test::check("V15 a very long name is shortened, keeping its extension",
                cut.size() <= 150 && cut.size() > 4 &&
                    cut.compare(cut.size() - 4, 4, ".wav") == 0,
                std::to_string(cut.size()) + " bytes");

    return test::summary("unicode paths");
}
