// Regression tests for the reasoning budget.
//
// The bug: one max_tokens bounded the whole stream, so a model that thought for
// two thousand tokens had nothing left to answer with. The run then failed with
// "the model used the whole answer budget on reasoning" -- after every section
// of a long recording had already been paid for -- and the advice it gave,
// raise the answer length, shrank the slice budget and made the next attempt
// worse. Reasoning now spends an allowance of its own and an overrun is closed
// rather than fatal.
//
// The model is a callback here, so these run without a GGUF: a token is
// whatever string the test feeds in.

#include "llm/reasoning.h"

#include <string>
#include <vector>

#include "check.h"

using namespace transcriptor::llm;
using test::check;

namespace {

// Feeds pieces in order and reports what came back, so a test can say what the
// stream looked like rather than how the state machine got there.
struct Run {
    int  think_used  = 0;
    int  answer_used = 0;
    int  forced      = 0;   // times the caller was told to close the block
    bool stopped     = false;
    std::size_t consumed = 0;
};

Run drive(ReasoningBudget& budget, const std::vector<std::string>& pieces) {
    Run r;
    for (const auto& piece : pieces) {
        ++r.consumed;
        const ReasoningStep step = budget.feed(piece);
        if (step == ReasoningStep::ForceClose) ++r.forced;
        if (step == ReasoningStep::Stop) { r.stopped = true; break; }
    }
    r.think_used  = budget.think_used();
    r.answer_used = budget.answer_used();
    return r;
}

// One token per word, which is close enough to how a model streams for these.
std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t sp = text.find(' ', at);
        if (sp == std::string::npos) { out.push_back(text.substr(at)); break; }
        out.push_back(text.substr(at, sp - at + 1));
        at = sp + 1;
    }
    return out;
}

void thinking_does_not_spend_the_answer() {
    ReasoningBudget budget(/*allow_thinking=*/true, /*think=*/10, /*answer=*/5);
    // Six tokens of reasoning, then the answer. The answer budget must still be
    // whole when the block closes: this is the whole point of the change.
    Run r = drive(budget, {"<think>", "the", "meeting", "was", "about",
                           "</think>", "Notes", "follow"});
    check("reasoning is charged to the think budget", r.think_used == 6,
          "think_used=" + std::to_string(r.think_used));
    check("the answer budget is untouched by reasoning", r.answer_used == 2,
          "answer_used=" + std::to_string(r.answer_used));
    check("a closed block needs no forcing", r.forced == 0);
}

void an_overrun_is_closed_not_fatal() {
    ReasoningBudget budget(/*allow_thinking=*/true, /*think=*/4, /*answer=*/3);
    Run r = drive(budget, {"<think>", "still", "thinking", "and", "more",
                           "and", "more", "and", "more"});
    check("an overrunning think block is force-closed once", r.forced == 1,
          "forced=" + std::to_string(r.forced));
    check("reasoning stops at its budget", r.think_used == 4,
          "think_used=" + std::to_string(r.think_used));
    // Everything after the forced close is the answer, so the model still gets
    // its full allowance to write one.
    check("the answer budget survives the overrun", r.answer_used == 3,
          "answer_used=" + std::to_string(r.answer_used));
    check("generation stops once the answer is spent", r.stopped);
}

void a_plain_answer_is_never_mistaken_for_reasoning() {
    // A model allowed to think does not have to. Force-closing here would
    // splice a stray </think> into the middle of a perfectly good summary.
    ReasoningBudget budget(/*allow_thinking=*/true, /*think=*/2, /*answer=*/10);
    Run r = drive(budget, words("The team agreed to ship on Friday"));
    check("a straight answer is never force-closed", r.forced == 0);
    check("no reasoning is charged for it", r.think_used == 0,
          "think_used=" + std::to_string(r.think_used));
    check("every token counts against the answer", r.answer_used == 7,
          "answer_used=" + std::to_string(r.answer_used));
}

void the_opening_tag_may_arrive_in_pieces() {
    // Tokenizers split "<think>" differently; the tag is not one token
    // everywhere, and it usually follows a newline or two.
    ReasoningBudget budget(true, /*think=*/8, /*answer=*/4);
    Run r = drive(budget, {"\n", "<", "th", "ink", ">", "hmm", "</th", "ink>",
                           "Done"});
    check("a split opening tag is still recognized", r.think_used > 0,
          "think_used=" + std::to_string(r.think_used));
    check("a split closing tag still ends the block", r.answer_used == 1,
          "answer_used=" + std::to_string(r.answer_used));
    check("no forcing was needed", r.forced == 0);
}

void thinking_off_charges_everything_to_the_answer() {
    // The prompt was prefilled with a closed <think></think> pair, so the model
    // is already past reasoning and there is nothing to watch for.
    ReasoningBudget budget(/*allow_thinking=*/false, /*think=*/0, /*answer=*/3);
    Run r = drive(budget, {"a", "b", "c", "d"});
    check("every token is answer when thinking is off", r.answer_used == 3,
          "answer_used=" + std::to_string(r.answer_used));
    check("it stops on the answer budget", r.stopped && r.consumed == 3,
          "consumed=" + std::to_string(r.consumed));
    check("nothing is charged to reasoning", r.think_used == 0);
}

void a_finished_answer_owes_nothing() {
    // answer_left() is what tells a summary that ended on its budget from one
    // the context window cut off mid-sentence. Getting that backwards is what
    // used to save half-written summaries to summary.txt as though they were
    // finished.
    ReasoningBudget spent(false, 0, 2);
    drive(spent, {"a", "b"});
    check("a spent budget owes nothing", spent.answer_left() == 0,
          "left=" + std::to_string(spent.answer_left()));

    ReasoningBudget partial(false, 0, 10);
    drive(partial, {"a", "b"});
    check("an unfinished answer still owes tokens", partial.answer_left() == 8,
          "left=" + std::to_string(partial.answer_left()));
}

void whole_blocks_come_off_the_answer() {
    check("a finished block is removed",
          strip_reasoning("<think>weighing it up</think>\nThe notes") ==
              "The notes");
    check("an unterminated block takes the rest with it",
          strip_reasoning("Answer<think>and then it was cut off").empty() ==
              false);
    check("reasoning-only output leaves nothing",
          strip_reasoning("<think>only ever thought").empty());
    check("a word starting with think is not a tag",
          strip_reasoning("the thinker said so") == "the thinker said so");
}

// V5: a server that renders the chat template -- LM Studio, llama-server with
// --jinja, vLLM -- sends DeepSeek-R1 and QwQ output without the opening tag,
// because the template put it in the prompt. The reasoning reached the summary
// pane and summary.txt, closing tag and all.
void a_closing_tag_alone_still_ends_the_reasoning() {
    check("V5 reasoning before an orphan </think> is removed",
          strip_reasoning("Let me weigh the decisions first...\n</think>\n\n"
                          "## Summary\nBeta ships Friday.") ==
              "## Summary\nBeta ships Friday.");
    check("V5 the same for </thinking>",
          strip_reasoning("weighing\n</thinking>\nThe notes") == "The notes");
    check("V5 the tag is matched without regard to case",
          strip_reasoning("weighing</THINK>The notes") == "The notes");
    check("V5 a well-formed block after the answer's reasoning is removed too",
          strip_reasoning("weighing</think>The notes<think>again</think>") ==
              "The notes");
}

}  // namespace

int main() {
    thinking_does_not_spend_the_answer();
    an_overrun_is_closed_not_fatal();
    a_plain_answer_is_never_mistaken_for_reasoning();
    the_opening_tag_may_arrive_in_pieces();
    thinking_off_charges_everything_to_the_answer();
    a_finished_answer_owes_nothing();
    whole_blocks_come_off_the_answer();
    a_closing_tag_alone_still_ends_the_reasoning();
    return test::summary("reasoning");
}
