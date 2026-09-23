#include "llm/reasoning.h"

#include <algorithm>

namespace transcriptor::llm {

namespace {

constexpr char kOpen[]  = "<think";
constexpr char kClose[] = "</think>";
constexpr std::size_t kOpenLen  = sizeof(kOpen) - 1;
constexpr std::size_t kCloseLen = sizeof(kClose) - 1;

// Wide enough that a closing tag split across several tokens still lands whole
// inside the window, with room to spare.
constexpr std::size_t kTailKeep = 4 * kCloseLen;

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string ltrim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b);
}

// Case-insensitive find, ASCII only — which is all the tag names are.
std::size_t ifind(const std::string& hay, const std::string& needle, std::size_t from) {
    const auto lower = [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    };
    if (needle.empty() || needle.size() > hay.size()) return std::string::npos;
    for (std::size_t i = from; i + needle.size() <= hay.size(); ++i) {
        std::size_t j = 0;
        while (j < needle.size() && lower(hay[i + j]) == lower(needle[j])) ++j;
        if (j == needle.size()) return i;
    }
    return std::string::npos;
}

}  // namespace

std::string strip_reasoning(const std::string& text) {
    std::string out = text;

    for (const char* name : {"think", "thinking"}) {
        const std::string open  = std::string("<") + name;
        const std::string close = std::string("</") + name + ">";

        for (std::size_t at = ifind(out, open, 0); at != std::string::npos;
             at = ifind(out, open, at)) {
            // Only a real tag: "<think>" or "<think ...>", never "<thinker".
            const std::size_t gt = out.find('>', at + open.size());
            if (gt == std::string::npos) { out.erase(at); break; }
            const char after = out[at + open.size()];
            if (after != '>' && after != ' ' && after != '\t' && after != '\n') {
                at += open.size();
                continue;
            }

            const std::size_t end = ifind(out, close, gt + 1);
            // No closing tag means generation was cut off mid-reasoning, so
            // everything from here on is chain of thought, not an answer.
            if (end == std::string::npos) { out.erase(at); break; }
            out.erase(at, (end + close.size()) - at);
        }

        // A closing tag with no opening one. Chat templates for DeepSeek-R1,
        // QwQ and their kind open the block themselves, in the prompt, so a
        // server that renders the template sends back only "...</think>" and
        // then the answer. Everything before the last such tag is reasoning.
        for (std::size_t at = ifind(out, close, 0), last = std::string::npos;;
             at = ifind(out, close, at + close.size())) {
            if (at == std::string::npos) {
                if (last != std::string::npos) out.erase(0, last + close.size());
                break;
            }
            last = at;
        }
    }

    return trim(out);
}

ReasoningBudget::ReasoningBudget(bool allow_thinking, int think_budget,
                                 int answer_budget)
    : state_(allow_thinking && think_budget > 0 ? State::Undecided
                                                : State::Answering),
      think_budget_(std::max(0, think_budget)),
      answer_budget_(std::max(1, answer_budget)) {}

ReasoningStep ReasoningBudget::feed(const std::string& piece) {
    if (state_ == State::Undecided) {
        ++pending_;
        head_ += piece;

        // Leading whitespace decides nothing; templates and models both emit it.
        const std::string seen = ltrim(head_);
        if (seen.empty()) return ReasoningStep::Continue;

        const std::size_t n = std::min(seen.size(), kOpenLen);
        if (seen.compare(0, n, kOpen, n) != 0) {
            // It answered straight away. Everything so far was the answer, and
            // no </think> is ever coming — force-closing here would splice a
            // stray tag into the middle of a perfectly good first sentence.
            state_ = State::Answering;
            head_.clear();
            return count_answer(pending_);
        }
        if (seen.size() < kOpenLen) return ReasoningStep::Continue;  // still ambiguous

        state_      = State::Thinking;
        think_used_ = pending_;
        head_.clear();
        keep_tail(seen);
        return check_close();   // "<think></think>" can arrive as one token
    }

    if (state_ == State::Thinking) {
        ++think_used_;
        keep_tail(piece);
        return check_close();
    }

    return count_answer(1);
}

ReasoningStep ReasoningBudget::check_close() {
    if (tail_.find(kClose, 0, kCloseLen) != std::string::npos) {
        state_ = State::Answering;
        tail_.clear();
        return ReasoningStep::Continue;
    }
    if (think_used_ >= think_budget_) {
        // Out of allowance. The caller closes the block for the model, which
        // then writes its answer with the whole answer budget still intact.
        state_ = State::Answering;
        tail_.clear();
        return ReasoningStep::ForceClose;
    }
    return ReasoningStep::Continue;
}

ReasoningStep ReasoningBudget::count_answer(int tokens) {
    answer_used_ += tokens;
    return answer_used_ >= answer_budget_ ? ReasoningStep::Stop
                                          : ReasoningStep::Continue;
}

void ReasoningBudget::keep_tail(const std::string& piece) {
    tail_ += piece;
    if (tail_.size() > kTailKeep) tail_.erase(0, tail_.size() - kTailKeep);
}

}  // namespace transcriptor::llm
