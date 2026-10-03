// The subtitle and audio-description credits Whisper hallucinates over silence,
// and the text filter that takes them back out.
//
// Split out of whisper_stt.cpp so it can be tested without a model, an audio
// device or a GPU: the whole thing is string work on an already-decoded segment.
#pragma once

#include <string>

#include "stt/whisper_stt.h"

namespace transcriptor::stt {

// Reduces text to a comparison key: Turkish letters mapped to ASCII, case
// dropped, and everything that isn't a letter or digit removed -- so
// "Altyazı M.K.", "ALTYAZI: M.K." and "altyazi mk" all fold to "altyazimk".
std::string fold(const std::string& s);

// True when a folded key is a credit nobody says out loud: subtitle-site
// credits and the audio-description disclaimer. Matched against the *whole* of
// a segment (or of a trailing run of words), never as a substring of running
// speech -- "Altyazıları açar mısın?" has to survive.
bool is_credit(const std::string& key);

// Takes the credits out of one decoded segment. Returns false when nothing is
// left to keep, which is what a segment that was only ever a credit reduces to.
//
// `silence_removed` says the voice detector cut the silence out before the
// decode. The video sign-offs Whisper also invents ("Thanks for watching",
// "Abone olmayı unutmayın") are then left alone: it invents them over silence
// it no longer sees, and people genuinely say them.
bool strip_credits(TranscriptSegment& seg, bool silence_removed);

}  // namespace transcriptor::stt
