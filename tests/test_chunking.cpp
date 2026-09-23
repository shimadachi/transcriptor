// Regression tests for the summary chunking (report findings R8, V8, V9).
//
// The bug: sections were sliced by a fixed chars-per-token guess and never
// measured, so token-dense text overflowed the context window on the very first
// section -- after the user had already waited for the model to load.
//
// "Does it fit?" arrives as a callback, so these run without a GGUF: a
// synthetic tokenizer stands in, and bytes_per_token is what makes text dense.

#include "llm/chunking.h"

#include <string>
#include <vector>

#include "check.h"

using namespace transcriptor::llm;

namespace {

// Mirrors LlamaBackend::fits(): the rendered prompt is the template overhead,
// the system prompt and the user message, and the answer must still fit after.
struct Window {
    int         n_ctx;
    int         max_tokens;
    double      bytes_per_token;
    std::string system;
    std::string context;
    std::size_t template_overhead = 40;

    int tokens(const std::string& s) const {
        return static_cast<int>(static_cast<double>(s.size()) / bytes_per_token) + 1;
    }
    bool fits(const std::string& section) const {
        const std::string user = "CONTEXT:\n" + context + "\nTRANSCRIPT:\n" + section;
        const std::string rendered =
            std::string(template_overhead, 'x') + system + user;
        return tokens(rendered) + max_tokens <= n_ctx;
    }
    std::size_t budget_chars() const {
        const int budget_tokens = n_ctx - max_tokens - 512;
        return budget_tokens > 0 ? static_cast<std::size_t>(budget_tokens) * 5 / 2 : 0;
    }
};

bool valid_utf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2
                    : (c >> 3) == 0x1E ? 3 : -1;
        if (n < 0 || i + n >= s.size()) return false;
        for (int k = 1; k <= n; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += n + 1;
    }
    return true;
}

std::string make_text(std::size_t bytes, std::size_t line_len) {
    std::string out;
    while (out.size() < bytes) {
        out.append(line_len, 'a');
        out.push_back('\n');
    }
    return out;
}

// The property that matters: nothing is dropped and nothing overflows.
void every_section_fits(const char* name, Window w, std::size_t bytes,
                        std::size_t line_len) {
    const std::string text = make_text(bytes, line_len);
    std::vector<std::string> parts;
    try {
        parts = split_to_fit(text, w.budget_chars(),
                             [&](const std::string& s) { return w.fits(s); });
    } catch (const SectionTooLarge&) {
        test::check(name, false, "split_to_fit gave up");
        return;
    }
    std::size_t kept = 0;
    bool all_fit = !parts.empty();
    for (const std::string& p : parts) {
        kept += p.size();
        if (!w.fits(p)) all_fit = false;
    }
    // Splitting drops nothing but trailing whitespace-only remainders.
    const bool complete = kept >= text.size() * 9 / 10;
    test::check(name, all_fit && complete,
                std::to_string(parts.size()) + " sections, " +
                    std::to_string(kept) + "/" + std::to_string(text.size()) +
                    " bytes kept");
}

}  // namespace

int main() {
    // -- split_transcript ---------------------------------------------------
    {
        auto one = split_transcript("short", 100);
        test::check("text under the budget stays whole",
                    one.size() == 1 && one[0] == "short");

        auto many = split_transcript(make_text(1000, 20), 100);
        bool bounded = !many.empty();
        for (const auto& c : many) bounded = bounded && c.size() <= 100;
        test::check("no chunk exceeds the byte budget", bounded,
                    std::to_string(many.size()) + " chunks");

        // A line longer than the whole budget has nowhere good to break.
        auto huge = split_transcript(std::string(500, 'x'), 100);
        bool cut = huge.size() == 5;
        for (const auto& c : huge) cut = cut && c.size() <= 100;
        test::check("an unbroken line is cut to the budget", cut,
                    std::to_string(huge.size()) + " chunks");

        test::check("a zero budget yields nothing rather than looping",
                    split_transcript("anything", 0).empty());
    }

    // -- V8: a transcript that is one long line --------------------------------
    // Without speaker separation the transcript is joined into a single line,
    // so "prefer line boundaries" never applied and every section ended at a
    // raw byte offset: through a word, and often through a Turkish letter.
    {
        std::string line;
        for (int i = 0; i < 60; ++i) {
            line += "Bugün toplantıda bütçe, müşteri şikâyetleri ve üçüncü çeyrek "
                    "hedefleri görüşüldü. ";
        }
        int split_chars = 0, split_words = 0, broken = 0;
        for (std::size_t budget = 900; budget < 1300; ++budget) {
            const auto parts = split_transcript(line, budget);
            std::string joined;
            for (std::size_t k = 0; k < parts.size(); ++k) {
                if (!valid_utf8(parts[k])) ++split_chars;
                if (k > 0 && parts[k - 1].back() != ' ' && parts[k].front() != ' ') {
                    ++split_words;
                }
                if (parts[k].size() > budget) ++broken;
                joined += parts[k];
            }
            if (joined != line) ++broken;
        }
        test::check("V8 no section boundary splits a character", split_chars == 0,
                    std::to_string(split_chars) + " sections over 400 budgets");
        test::check("V8 no section boundary splits a word", split_words == 0,
                    std::to_string(split_words) + " boundaries over 400 budgets");
        test::check("V8 sections stay within the budget and lose nothing",
                    broken == 0, std::to_string(broken) + " budgets went wrong");
    }

    // -- split_to_fit -------------------------------------------------------
    // Ordinary English density: the 2.5 chars/token guess is pessimistic and
    // almost nothing needs re-splitting.
    every_section_fits("ordinary text: every section fits",
                       Window{8192, 2048, 3.5, std::string(300, 's'), ""},
                       400000, 80);

    // R8's actual failure: text that tokenizes far denser than the guess. The
    // old byte-only split handed the model a section it could not take.
    every_section_fits("token-dense text is re-split until it fits",
                       Window{8192, 2048, 1.0, std::string(300, 's'), ""},
                       400000, 80);

    // A long per-template context rides on every single section.
    every_section_fits("dense text with a large per-section context",
                       Window{8192, 2048, 0.8, std::string(300, 's'),
                              std::string(3000, 'c')},
                       200000, 40);

    every_section_fits("a single unbroken line still splits to fit",
                       Window{8192, 2048, 1.0, std::string(300, 's'), ""},
                       200000, 200000);

    every_section_fits("a tight window still resolves",
                       Window{4096, 1024, 2.0, std::string(200, 's'), ""},
                       100000, 60);

    // When the fixed part of the prompt is what does not fit, splitting the
    // transcript cannot help: say so instead of splitting for ever.
    {
        Window w{4096, 1024, 1.0, std::string(2000, 's'), std::string(4000, 'c')};
        bool threw = false;
        try {
            split_to_fit(make_text(50000, 80), 1000,
                         [&](const std::string& s) { return w.fits(s); });
        } catch (const SectionTooLarge&) { threw = true; }
        test::check("an oversized fixed prompt reports instead of looping", threw);
    }

    // A section that already fits must be handed back untouched.
    {
        Window w{8192, 2048, 3.5, "sys", ""};
        auto parts = split_to_fit("one small section", 10000,
                                  [&](const std::string& s) { return w.fits(s); });
        test::check("a transcript that already fits is not split",
                    parts.size() == 1 && parts[0] == "one small section");
    }

    // -- V9: the merge keeps what the user said about the recording -----------
    // The final pass over the section notes dropped the context as "already in
    // the notes" -- but the section passes only note topics, decisions and
    // actions, so a long recording's summary came out without the meeting's
    // title, without who was in it, and without the template's standing
    // instructions.
    {
        SummaryRequest work;
        work.template_id     = "standup";
        work.language        = "tr";
        work.system_override = "Summarize per person.";
        work.context         = "Başlık: Q3 bütçe\nKatılımcılar: Ayşe, Can";
        work.transcript      = "the whole long transcript";
        const SummaryRequest merge = merge_request(work, "- notes from each section");
        test::check("V9 the merge keeps the context", merge.context == work.context,
                    merge.context);
        test::check("V9 the notes stand in for the transcript",
                    merge.transcript == "- notes from each section");
        test::check("V9 the template, its prompt and the language carry over",
                    merge.template_id == "standup" && merge.language == "tr" &&
                        merge.system_override == work.system_override);
    }

    return test::summary("chunking");
}
