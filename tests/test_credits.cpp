// Regression tests for hallucinated credits surviving into a saved transcript.
//
// The bug: a recording that opens on silence gets "Bu dizinin betimlemesi TRT
// tarafından Sesli Betimleme Derneğine yaptırılmıştır." in the first window --
// a broadcaster's audio-description disclaimer Whisper has memorised. The
// filter knew about subtitle credits but not this family, so the line was
// saved as if someone had said it.
//
// The real fix is the voice detector in whisper_stt.cpp, which keeps silence
// away from the decoder; that needs a model, a GPU and a recording, so what is
// checked here is the filter standing behind it -- and, just as importantly,
// that ordinary speech still gets through untouched.

#include "stt/credits.h"

#include <string>
#include <vector>

#include "check.h"

using namespace transcriptor::stt;

namespace {

// A segment as whisper_stt.cpp builds one: words carry their leading space.
TranscriptSegment segment(const std::vector<std::string>& words) {
    TranscriptSegment seg;
    double t = 0.0;
    for (const std::string& w : words) {
        Word word;
        word.text  = " " + w;
        word.start = t;
        word.end   = t + 0.5;
        seg.text  += word.text;
        seg.words.push_back(word);
        t += 0.5;
    }
    seg.start = 0.0;
    seg.end   = t;
    // trim() in whisper_stt.cpp strips the leading space before this is reached.
    if (!seg.text.empty()) seg.text.erase(0, 1);
    return seg;
}

void dropped(const char* what, const std::vector<std::string>& words) {
    TranscriptSegment seg = segment(words);
    const bool kept = strip_credits(seg);
    test::check(what, !kept, kept ? "kept: " + seg.text : "");
}

void kept_whole(const char* what, const std::vector<std::string>& words) {
    TranscriptSegment seg = segment(words);
    const std::string before = seg.text;
    const bool kept = strip_credits(seg);
    const bool ok = kept && seg.text == before;
    test::check(what, ok,
                ok ? "" : kept ? "became: " + seg.text : "dropped entirely");
}

void trimmed_to(const char* what, const std::vector<std::string>& words,
                const std::string& want) {
    TranscriptSegment seg = segment(words);
    const bool kept = strip_credits(seg);
    const bool ok = kept && seg.text == want;
    test::check(what, ok,
                ok ? "" : kept ? "got: " + seg.text : "dropped entirely");
}

}  // namespace

int main() {
    // -- the disclaimer that prompted this ------------------------------------
    // Nine words, and the tail varies by broadcaster, so neither an exact line
    // nor the old eight-word trailing scan would have caught it.
    dropped("TRT audio-description disclaimer",
            {"Bu", "dizinin", "betimlemesi", "TRT", "tarafından", "Sesli",
             "Betimleme", "Derneğine", "yaptırılmıştır."});

    dropped("the same disclaimer for a film",
            {"Bu", "filmin", "betimlemesi", "Sesli", "Betimleme", "Derneği",
             "tarafından", "yapılmıştır."});

    dropped("the same disclaimer for a programme",
            {"Bu", "programın", "betimlemesi", "yapılmıştır."});

    dropped("the same disclaimer for an episode",
            {"Bu", "bölümün", "betimlemesi", "yapılmıştır."});

    // -- the subtitle credits that were already known -------------------------
    dropped("Altyazı M.K.", {"Altyazı", "M.K."});
    dropped("case and punctuation folded", {"ALTYAZI:", "m.k."});
    dropped("English subtitle-site credit",
            {"Subtitles", "by", "the", "Amara.org", "community"});

    // -- a credit stuck to the end of real speech -----------------------------
    trimmed_to("trailing credit trimmed, speech kept",
               {"Görüşmek", "üzere", "arkadaşlar.", "Altyazı", "M.K."},
               "Görüşmek üzere arkadaşlar.");

    trimmed_to("trailing disclaimer trimmed, speech kept",
               {"Herkese", "iyi", "çalışmalar.", "Bu", "dizinin", "betimlemesi",
                "TRT", "tarafından", "Sesli", "Betimleme", "Derneğine",
                "yaptırılmıştır."},
               "Herkese iyi çalışmalar.");

    // -- speech that must survive ---------------------------------------------
    // The credit words do occur in real sentences. Whole-segment and trailing
    // matching is what keeps these; a substring search would eat them.
    kept_whole("a question about subtitles",
               {"Altyazıları", "açar", "mısın?"});

    kept_whole("betimleme discussed rather than credited",
               {"Sesli", "betimleme", "desteği", "ekleyelim", "mi?"});

    kept_whole("a sentence that merely starts the same way",
               {"Bu", "dizinin", "betimlemesi", "hakkında", "ne",
                "düşünüyorsunuz?"});

    kept_whole("thanks in the middle of a meeting",
               {"Teşekkürler,", "sonra", "konuşuruz."});

    return test::summary("credits");
}
