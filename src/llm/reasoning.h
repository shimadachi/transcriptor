// Chain-of-thought handling, kept free of llama.cpp for the same reason
// llm/chunking.cpp is: the interesting logic is decisions about a token stream,
// and those are worth testing without a GGUF, a GPU or a model download.
//
// Two jobs live here:
//   * strip_reasoning() takes a finished <think> block off an answer.
//   * ReasoningBudget watches the stream as it is produced, so reasoning spends
//     an allowance of its own instead of the answer's.
#pragma once

#include <string>

namespace transcriptor::llm {

// Drop the chain-of-thought block reasoning models emit before their answer.
// Qwen and DeepSeek-R1 wrap it in <think>...</think> and expect the caller to
// discard it; left in, it reaches the summary pane and summary.txt verbatim.
// Applies to both backends, since an OpenAI-compatible server fronting one of
// those models passes the tags straight through.
std::string strip_reasoning(const std::string& text);

// What the caller should do with the token it just fed in.
enum class ReasoningStep {
    Continue,    // nothing to do
    ForceClose,  // reasoning has spent its allowance: feed the model a closing
                 // </think> and keep sampling — what comes next is the answer
    Stop,        // the answer budget is spent; this token was the last one owed
};

// The token accounting for one generation.
//
// The problem it solves: a single max_tokens used to bound the whole stream, so
// a model that thought for two thousand tokens had nothing left to answer with
// and the run died with "the model used the whole answer budget on reasoning" —
// after the thinking had already been paid for. Here reasoning gets
// think_budget tokens of its own, the answer keeps all of answer_budget, and an
// overrun is closed rather than fatal.
//
// Feed it the text of each sampled token, in order, and act on what it returns.
class ReasoningBudget {
public:
    // allow_thinking == false means the prompt was prefilled with an empty
    // <think></think> pair, so the model is already past reasoning and every
    // token counts against the answer.
    ReasoningBudget(bool allow_thinking, int think_budget, int answer_budget);

    ReasoningStep feed(const std::string& piece);

    bool thinking() const { return state_ == State::Thinking; }
    int  think_used() const { return think_used_; }
    int  answer_used() const { return answer_used_; }

    // Answer tokens still owed. Zero means the model stopped because its budget
    // ended, which is a finished summary; anything else means it was cut short.
    int answer_left() const {
        return answer_budget_ > answer_used_ ? answer_budget_ - answer_used_ : 0;
    }

private:
    // Undecided is the wait for the opening tag: a model told it may think does
    // not have to, and a normal answer must not be mistaken for chain of
    // thought and force-closed in the middle of its first sentence.
    enum class State { Undecided, Thinking, Answering };

    ReasoningStep count_answer(int tokens);
    ReasoningStep check_close();
    void          keep_tail(const std::string& piece);

    State state_;
    int   think_budget_;
    int   answer_budget_;
    int   think_used_  = 0;
    int   answer_used_ = 0;
    int   pending_     = 0;   // tokens seen while Undecided
    std::string head_;        // the opening bytes, until the tag is settled
    std::string tail_;        // a window wide enough to catch a split </think>
};

}  // namespace transcriptor::llm
