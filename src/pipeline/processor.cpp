#include "pipeline/processor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>

#include "util/models.h"

namespace transcriptor::pipeline {

namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

double round2(double v) { return std::round(v * 100.0) / 100.0; }

}  // namespace

std::string fmt_ts(double seconds) {
    long total = std::max(0L, static_cast<long>(std::llround(seconds)));
    const long h = total / 3600;
    const long m = (total % 3600) / 60;
    const long s = total % 60;

    char buf[32];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", h, m, s);
    } else {
        std::snprintf(buf, sizeof(buf), "%02ld:%02ld", m, s);
    }
    return buf;
}

std::string ProcessResult::plain_text(const std::string& lang, bool with_ts) const {
    std::vector<std::string> parts;
    parts.reserve(lines.size());

    for (const Line& l : lines) {
        std::string out;
        if (with_ts) out += "[" + fmt_ts(l.start) + "] ";
        if (diarized) out += diarize::speaker_name(l.speaker, lang) + ": ";
        out += trim(l.text);
        parts.push_back(std::move(out));
    }

    // Speaker-labelled or timestamped output is one line per turn; a plain
    // transcript reads better as a single flowing paragraph.
    const std::string joiner = (diarized || with_ts) ? "\n" : " ";
    std::string text;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) text += joiner;
        text += parts[i];
    }
    return trim(text);
}

nlohmann::json ProcessResult::to_json(const std::string& lang) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const Line& l : lines) {
        arr.push_back({
            {"speaker", l.speaker < 0 ? nlohmann::json(nullptr)
                                      : nlohmann::json(l.speaker)},
            {"speaker_name", diarized
                                 ? nlohmann::json(diarize::speaker_name(l.speaker, lang))
                                 : nlohmann::json(nullptr)},
            {"text", trim(l.text)},
            {"start", round2(l.start)},
            {"end", round2(l.end)},
            {"ts", fmt_ts(l.start)},
        });
    }
    return {
        {"diarized", diarized},
        {"num_speakers", num_speakers},
        {"duration", round2(duration)},
        {"lines", arr},
        {"plain_text", plain_text(lang)},
    };
}

std::vector<Line> attribute(const std::vector<stt::TranscriptSegment>& segments,
                            const std::vector<diarize::Turn>& turns,
                            int* num_speakers) {
    // Speaker index of the turn that overlaps [s, e] most; -1 if none does.
    auto speaker_at = [&turns](double s, double e) -> int {
        int    best = -1;
        double best_overlap = 0.0;
        for (const diarize::Turn& t : turns) {
            const double overlap = std::min(e, t.end) - std::max(s, t.start);
            if (overlap > best_overlap) {
                best_overlap = overlap;
                best = t.speaker;
            }
        }
        return best;
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

OfflineProcessor::OfflineProcessor(Settings settings, DeviceInfo device)
    : settings_(std::move(settings)), device_(std::move(device)) {}

OfflineProcessor::~OfflineProcessor() = default;

void OfflineProcessor::unload() {
    if (transcriber_) transcriber_->unload();
    if (diarizer_) diarizer_->unload();
}

void OfflineProcessor::request_abort() {
    if (transcriber_) transcriber_->request_abort();
}

ProcessResult OfflineProcessor::run(const std::vector<float>& audio, int samplerate,
                                    const ProgressFn& progress) {
    auto report = [&progress](const std::string& phase) {
        return [&progress, phase](const std::string& message, double fraction) {
            if (progress) progress(phase, fraction, message);
        };
    };

    ProcessResult result;
    result.duration = samplerate > 0
                          ? static_cast<double>(audio.size()) / samplerate
                          : 0.0;

    // -- transcribe --------------------------------------------------------
    if (progress) progress("transcribe", -1.0, "");

    if (std::string err = models::ensure_whisper_model(settings_, report("transcribe"));
        !err.empty()) {
        throw std::runtime_error(err);
    }

    if (!transcriber_ || transcriber_->model_name() != settings_.whisper_model) {
        transcriber_ = std::make_unique<stt::WhisperTranscriber>(
            settings_.whisper_model, device_, settings_.language,
            settings_.stt_threads);
    }
    const auto segments = transcriber_->transcribe(
        audio, settings_.whisper_model_file(), report("transcribe"));

    // -- diarize (optional) ------------------------------------------------
    const bool want_diarization =
        settings_.enable_diarization && diarize::Diarizer::supported();

    if (!want_diarization) {
        result.lines.reserve(segments.size());
        for (const stt::TranscriptSegment& s : segments) {
            result.lines.push_back({-1, s.text, s.start, s.end});
        }
        result.diarized = false;
        return result;
    }

    if (progress) progress("diarize", -1.0, "");

    if (std::string err = models::ensure_diarization_models(settings_,
                                                            report("diarize"));
        !err.empty()) {
        throw std::runtime_error(err);
    }

    if (!diarizer_) {
        diarizer_ = std::make_unique<diarize::Diarizer>(settings_, device_);
    }
    const auto turns = diarizer_->diarize(audio, samplerate, report("diarize"));

    // -- attribute ---------------------------------------------------------
    if (progress) progress("attribute", -1.0, "");
    result.lines = attribute(segments, turns, &result.num_speakers);
    result.diarized = true;
    return result;
}

}  // namespace transcriptor::pipeline
