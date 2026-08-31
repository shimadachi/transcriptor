#include "llm/summarizer.h"

#include "llm/llama_backend.h"
#include "llm/openai_backend.h"
#include "llm/templates.h"

namespace transcriptor::llm {

namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
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
    }

    return trim(out);
}

std::string resolve_system_prompt(const SummaryRequest& req) {
    const std::string override_prompt = trim(req.system_override);
    if (!override_prompt.empty()) return override_prompt;
    return system_prompt(req.template_id, req.language);
}

std::string build_user_message(const SummaryRequest& req) {
    const std::string transcript = trim(req.transcript);
    const std::string context    = trim(req.context);
    if (context.empty()) return transcript;

    const bool tr = (req.language == "tr");
    const std::string ctx_label = tr ? "--- Bağlam ---"     : "--- Context ---";
    const std::string tx_label  = tr ? "--- Transkript ---" : "--- Transcript ---";
    return ctx_label + "\n" + context + "\n\n" + tx_label + "\n" + transcript;
}

std::unique_ptr<Backend> make_backend(const Settings& settings,
                                      const DeviceInfo& device) {
    if (settings.llm_backend == "remote") {
        return make_openai_backend(settings);
    }
    return make_llama_backend(settings, device);
}

}  // namespace transcriptor::llm
