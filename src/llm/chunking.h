// Cutting a long transcript into pieces that fit a model's context window.
//
// Kept apart from llama_backend.cpp, and free of llama.cpp, for one reason: the
// question "does this fit?" is the only part that needs a loaded model, so it
// arrives as a callback. Everything else is string arithmetic that can be
// tested without a GGUF on disk -- and the failure mode this code exists to
// prevent (a section that overflows the window after every earlier section has
// already been paid for) is expensive enough to be worth testing.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "llm/summarizer.h"

namespace transcriptor::llm {

// A section smaller than this that still does not fit means the fixed part of
// the prompt (system prompt, context) is what will not fit, and splitting the
// transcript further cannot help.
inline constexpr std::size_t kMinSectionChars = 256;

// Split into chunks of at most `max_chars`, preferring line boundaries so a
// speaker turn is never cut in half. A single line longer than the budget is
// cut mid-sentence, because there is nothing better to do with it.
std::vector<std::string> split_transcript(const std::string& text,
                                          std::size_t max_chars);

// Does this section, rendered as the prompt it will actually become, fit with
// room left for the answer?
using FitsFn = std::function<bool(const std::string& section)>;

// Split so that every returned section satisfies `fits`. `budget_chars` is only
// a starting guess -- a chars-per-token ratio is pessimistic for English and
// optimistic for token-dense text -- so each slice is measured and re-split
// when it is still too big.
//
// Throws SectionTooLarge when even a section of kMinSectionChars does not fit,
// which means the transcript is not what is too big.
std::vector<std::string> split_to_fit(const std::string& text,
                                      std::size_t budget_chars,
                                      const FitsFn& fits);

// The request for the pass that merges section notes into the summary. The
// notes stand in for the transcript; everything else carries over -- above all
// the context, meaning the template's own and the title, participants and
// notes typed for this run. The section passes are told only to note topics,
// decisions and actions, so a merge that dropped the context as "already in
// the notes" wrote a long recording's summary without the meeting's name or
// who was in it, and without the template's standing instructions.
SummaryRequest merge_request(const SummaryRequest& original, const std::string& notes);

// Raised by split_to_fit when no amount of splitting can help. Translated into
// a user-facing SummarizerError by the caller, which knows the language.
struct SectionTooLarge {};

}  // namespace transcriptor::llm
