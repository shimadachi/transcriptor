// Regression tests for saved transcript/summary versions in the library.
//
// Re-running a model over an archived recording can either replace what is
// there or sit beside it under a name. The name becomes a middle segment of a
// file name — transcript.<name>.json — so two things have to hold: a name can
// never reach out of its own segment, and the scan that lists what is on disk
// has to tell the original from the named ones and from files that merely look
// similar.
//
// No models, no server: this is the file layer, and it runs on a scratch dir.

#include "util/library.h"

#include <cstdio>
#include <fstream>
#include <string>

#include "check.h"

using namespace transcriptor;
using test::check;

namespace {

library::fs::path g_scratch;

void touch(const library::fs::path& p, const std::string& body = "x") {
    std::ofstream f(p);
    f << body;
}

std::string names_of(const std::vector<library::Variant>& vs) {
    std::string out = "[";
    for (std::size_t i = 0; i < vs.size(); ++i) {
        if (i) out += ", ";
        out += "\"" + vs[i].name + "\"";
    }
    return out + "]";
}

void test_names_stay_in_their_own_segment() {
    check("a plain name is usable", library::valid_variant("second pass"));
    check("dashes and underscores are fine", library::valid_variant("v2_take-3"));

    // A dot would end the segment and let the name pick the extension; a
    // separator would leave the session folder altogether.
    check("a dot is refused", !library::valid_variant("v1.2"));
    check("a slash is refused", !library::valid_variant("../escape"));
    check("a backslash is refused", !library::valid_variant("a\\b"));
    check("a colon is refused", !library::valid_variant("c:name"));
    check("an empty name is not a name", !library::valid_variant(""));
    check("surrounding spaces are refused", !library::valid_variant(" pad "));
    check("control characters are refused", !library::valid_variant(std::string("a\nb")));
}

void test_file_names() {
    const library::fs::path dir = "/tmp/session";
    check("no name gives the original transcript",
          library::transcript_json_file(dir, "").filename() == "transcript.json");
    check("no name gives the original summary",
          library::summary_file(dir, "").filename() == "summary.txt");
    check("a name becomes the middle segment",
          library::transcript_txt_file(dir, "second pass").filename() ==
              "transcript.second pass.txt",
          paths::to_utf8(library::transcript_txt_file(dir, "second pass").filename()));
    // An unusable name must not be smuggled into a path: it falls back to the
    // original rather than producing "summary.../../etc.txt".
    check("a refused name does not reach the path",
          library::summary_file(dir, "../etc").filename() == "summary.txt",
          paths::to_utf8(library::summary_file(dir, "../etc").filename()));
}

void test_scan_lists_originals_first() {
    const library::fs::path dir = g_scratch / "session";
    std::error_code ec;
    library::fs::create_directories(dir, ec);

    touch(dir / "transcript.txt");
    touch(dir / "transcript.json");
    touch(dir / "transcript.second pass.txt");
    touch(dir / "transcript.second pass.json");
    touch(dir / "transcript.notes only.txt");     // no .json beside it
    touch(dir / "summary.txt");
    touch(dir / "summary.shorter.txt");
    // Files that look like versions but are not: a different stem, and a name
    // with no separating dot at all.
    touch(dir / "transcripts.json");
    touch(dir / "transcriptfoo.txt");
    touch(dir / "audio.wav");

    const auto tx = library::transcript_variants(dir);
    check("every transcript is found and nothing else is", tx.size() == 3, names_of(tx));
    check("the original comes first", !tx.empty() && tx.front().name.empty(),
          names_of(tx));

    bool found_notes = false, notes_structured = true, pass_structured = false;
    for (const library::Variant& v : tx) {
        if (v.name == "notes only") { found_notes = true; notes_structured = v.structured; }
        if (v.name == "second pass") pass_structured = v.structured;
    }
    check("a transcript with only a .txt is still listed", found_notes, names_of(tx));
    check("...but is not marked structured", !notes_structured);
    check("one with a .json beside it is", pass_structured);

    const auto sums = library::summary_variants(dir);
    check("both summaries are found", sums.size() == 2, names_of(sums));
    check("the original summary comes first",
          !sums.empty() && sums.front().name.empty(), names_of(sums));
}

void test_a_json_only_transcript_is_not_lost() {
    const library::fs::path dir = g_scratch / "json-only";
    std::error_code ec;
    library::fs::create_directories(dir, ec);
    touch(dir / "transcript.json");            // saved without the .txt
    touch(dir / "transcript.later.json");

    const auto tx = library::transcript_variants(dir);
    check("a session kept only as JSON still has its transcripts",
          tx.size() == 2, names_of(tx));
    check("and both count as structured",
          tx.size() == 2 && tx[0].structured && tx[1].structured);
}

// A session is whatever the variant scan can find. Discovery used to ask only
// about transcript.json / transcript.txt / summary.txt, so a session whose
// originals had been deleted -- or one copied in holding only named output --
// was filtered out of the list, and everything it held became unreachable.
void test_a_session_of_only_named_versions_is_listed() {
    const library::fs::path root = g_scratch / "output";
    const library::fs::path dir = root / "2026-09-06_10-00-00";
    std::error_code ec;
    library::fs::create_directories(dir, ec);
    touch(dir / "transcript.second pass.txt", "what was actually said\n");
    touch(dir / "summary.shorter.txt", "the short version\n");

    const library::Entry e = library::describe(dir);
    check("a named transcript counts as having a transcript", e.has_transcript);
    check("a named summary counts as having a summary", e.has_summary);
    check("the preview comes from the version that exists",
          e.preview.rfind("what was actually said", 0) == 0, e.preview);

    const auto sessions = library::list(paths::to_utf8(root));
    check("the session is in the library", sessions.size() == 1,
          std::to_string(sessions.size()) + " session(s)");

    // A folder holding none of it is still not a session.
    const library::fs::path empty = root / "2026-09-06_11-00-00";
    library::fs::create_directories(empty, ec);
    touch(empty / "notes.md");
    check("a folder with no session artifacts is still skipped",
          library::list(paths::to_utf8(root)).size() == 1);

    // With the original there, it is the one previewed -- variants are listed
    // original-first and that is what the panel opens on.
    touch(dir / "transcript.txt", "the first attempt\n");
    check("the original is preferred for the preview",
          library::describe(dir).preview.rfind("the first attempt", 0) == 0,
          library::describe(dir).preview);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_library <scratch-dir>\n");
        return 2;
    }
    g_scratch = paths::from_utf8(argv[1]);
    std::error_code ec;
    library::fs::remove_all(g_scratch, ec);
    library::fs::create_directories(g_scratch, ec);

    test_names_stay_in_their_own_segment();
    test_file_names();
    test_scan_lists_originals_first();
    test_a_json_only_transcript_is_not_lost();
    test_a_session_of_only_named_versions_is_listed();
    return test::summary("library");
}
