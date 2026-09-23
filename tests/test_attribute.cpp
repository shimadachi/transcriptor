// Regression tests for speaker attribution (V10).
//
// The bug: a word whose timestamps overlapped no diarization turn became a
// line of its own under "Speaker ?". The diarizer leaves gaps between turns --
// it only bridges pauses under half a second -- and whisper's word timestamps
// land in them often enough, so one person's sentence came out as three lines
// with a mystery speaker in the middle, in the transcript and in what the
// summarizer was given.

#include <string>
#include <vector>

#include "check.h"
#include "pipeline/processor.h"

using namespace transcriptor;

namespace {

stt::TranscriptSegment segment(std::vector<stt::Word> words) {
    stt::TranscriptSegment seg;
    seg.start = words.front().start;
    seg.end   = words.back().end;
    for (const auto& w : words) seg.text += w.text;
    seg.words = std::move(words);
    return seg;
}

std::string describe(const std::vector<pipeline::Line>& lines) {
    std::string out;
    for (const auto& l : lines) {
        out += "[" + std::to_string(l.speaker) + "]" + l.text + " ";
    }
    return out;
}

void a_word_in_a_gap_stays_with_its_speaker() {
    const auto seg = segment({{0.00, 0.30, " We"}, {0.30, 0.70, " should"},
                              {0.70, 1.00, " ship"}, {2.05, 2.20, " on"},
                              {2.20, 2.60, " Friday."}});
    // One speaker, with a 0.3 s pause the diarizer left as a gap.
    const std::vector<diarize::Turn> turns = {{0.0, 2.0, 0}, {2.3, 5.0, 0}};
    int n = 0;
    const auto lines = pipeline::attribute({seg}, turns, &n);
    test::check("V10 one speaker's sentence stays one line",
                lines.size() == 1 && lines[0].speaker == 0, describe(lines));
    test::check("V10 and counts as one speaker", n == 1, std::to_string(n));
}

void a_word_between_two_speakers_goes_to_the_nearer() {
    const auto seg = segment({{0.0, 1.0, " Yes."}, {1.9, 2.1, " Right,"},
                              {2.6, 3.5, " so"}});
    const std::vector<diarize::Turn> turns = {{0.0, 1.2, 0}, {2.2, 4.0, 1}};
    const auto lines = pipeline::attribute({seg}, turns, nullptr);
    test::check("V10 a word in a gap between two speakers goes to the nearer one",
                lines.size() == 2 && lines[1].text == " Right, so", describe(lines));
}

void a_word_far_from_every_turn_stays_unknown() {
    const auto seg = segment({{0.0, 1.0, " Hello."}, {9.0, 9.5, " Music."}});
    const std::vector<diarize::Turn> turns = {{0.0, 1.5, 0}};
    const auto lines = pipeline::attribute({seg}, turns, nullptr);
    test::check("a word nowhere near a turn is still left unattributed",
                lines.size() == 2 && lines[1].speaker == -1, describe(lines));
}

void a_zero_length_word_inside_a_turn_belongs_to_it() {
    // Token timestamps occasionally come back with no length at all.
    const auto seg = segment({{1.0, 1.5, " Well"}, {1.8, 1.8, ","}, {2.0, 2.4, " yes."}});
    const std::vector<diarize::Turn> turns = {{0.5, 3.0, 0}};
    const auto lines = pipeline::attribute({seg}, turns, nullptr);
    test::check("V10 a zero-length word inside a turn belongs to it",
                lines.size() == 1 && lines[0].speaker == 0, describe(lines));
}

}  // namespace

int main() {
    a_word_in_a_gap_stays_with_its_speaker();
    a_word_between_two_speakers_goes_to_the_nearer();
    a_word_far_from_every_turn_stays_unknown();
    a_zero_length_word_inside_a_turn_belongs_to_it();
    return test::summary("attribute");
}
