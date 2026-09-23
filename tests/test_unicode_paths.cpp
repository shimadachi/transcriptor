// Regression test for model downloads into a folder named in a language the
// system code page cannot spell (V13).
//
// The download built its ".part" name through fs::path::string(), which on
// Windows converts to the ANSI code page and throws when a character has no
// place in it. The models folder lives under the user's profile, so a user
// named, say, Ağaoğlu on an English Windows got an exception -- thrown on the
// download thread, which is std::terminate: the app vanished mid-download.
//
// Meaningful on Windows, where the conversion happens; elsewhere it confirms
// the path is left as it was given.

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
    return test::summary("unicode paths");
}
