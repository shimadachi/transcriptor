// Speaker attribution: which diarization turn each transcribed word belongs
// to. Kept apart from processor.cpp, and free of whisper and sherpa, so the
// rule can be tested on its own -- it is plain interval arithmetic.
#include "pipeline/processor.h"

#include <algorithm>
#include <map>

namespace transcriptor::pipeline {

namespace {

// A word that overlaps no turn at all goes to the nearest one within this
// distance. The diarizer leaves gaps between turns -- it only bridges pauses
// under half a second -- and whisper's word timestamps land in them often
// enough; with no fallback each such word became a "Speaker ?" line of its
// own, cutting one person's sentence into three in the transcript and in
// what the summarizer read. Further out than this, "?" is the honest answer.
constexpr double kNearestTurnSec = 1.0;

}  // namespace

std::vector<Line> attribute(const std::vector<stt::TranscriptSegment>& segments,
                            const std::vector<diarize::Turn>& turns,
                            int* num_speakers) {
    // Speaker index of the turn that overlaps [s, e] most. A word that
    // overlaps none goes to the nearest turn within kNearestTurnSec; -1 only
    // when there is none that close.
    auto speaker_at = [&turns](double s, double e) -> int {
        int    best = -1;
        double best_overlap = 0.0;
        int    nearest = -1;
        double nearest_gap = kNearestTurnSec;
        for (const diarize::Turn& t : turns) {
            const double overlap = std::min(e, t.end) - std::max(s, t.start);
            if (overlap > best_overlap) {
                best_overlap = overlap;
                best = t.speaker;
            }
            // Positive for a turn wholly before or after the word; zero or
            // below for one it touches, or a zero-length word sitting inside.
            const double gap = std::max(t.start - e, s - t.end);
            if (overlap <= 0.0 && gap < nearest_gap) {
                nearest_gap = gap;
                nearest = t.speaker;
            }
        }
        return best >= 0 ? best : nearest;
    };

    // Renumber by first appearance so speaker 0 is whoever talks first.
    std::map<int, int> order;
    auto dense = [&order](int speaker) -> int {
        if (speaker < 0) return -1;
        auto it = order.find(speaker);
        if (it != order.end()) return it->second;
        const int idx = static_cast<int>(order.size());
        order[speaker] = idx;
        return idx;
    };

    std::vector<Line> lines;
    for (const stt::TranscriptSegment& seg : segments) {
        if (!seg.words.empty()) {
            for (const stt::Word& w : seg.words) {
                const int spk = dense(speaker_at(w.start, w.end));
                if (!lines.empty() && lines.back().speaker == spk) {
                    lines.back().text += w.text;
                    lines.back().end = w.end;
                } else {
                    lines.push_back({spk, w.text, w.start, w.end});
                }
            }
        } else {
            const int spk = dense(speaker_at(seg.start, seg.end));
            if (!lines.empty() && lines.back().speaker == spk) {
                lines.back().text += " " + seg.text;
                lines.back().end = seg.end;
            } else {
                lines.push_back({spk, " " + seg.text, seg.start, seg.end});
            }
        }
    }

    if (num_speakers) *num_speakers = static_cast<int>(order.size());
    return lines;
}

}  // namespace transcriptor::pipeline
