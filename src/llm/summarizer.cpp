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

}  // namespace

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
