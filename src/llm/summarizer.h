// Summarize a transcript with a local model.
//
// Two interchangeable backends:
//   * embedded — llama.cpp linked into this binary, loading a GGUF directly.
//     This is the compiled replacement for LM Studio; nothing to install.
//   * remote   — any OpenAI-compatible local server (LM Studio, Ollama,
//     llama-server) over /v1/models and /v1/chat/completions.
#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.h"
#include "device.h"

namespace transcriptor::llm {

class SummarizerError : public std::runtime_error {
public:
    explicit SummarizerError(const std::string& what) : std::runtime_error(what) {}
};

struct Availability {
    bool        ok = false;
    std::string message;   // ready-to-display, in the UI's language
};

// Streams status lines while a model loads or tokens are generated.
using ProgressFn = std::function<void(const std::string& message, double fraction)>;

struct SummaryRequest {
    std::string transcript;
    std::string context;            // "" = none
    std::string template_id = "meeting";
    std::string system_override;    // non-empty replaces the template prompt
    std::string language = "tr";
};

class Backend {
public:
    virtual ~Backend() = default;

    // Server/model reachable and usable?
    virtual Availability available() = 0;

    // Model ids the backend can offer (remote: from the server; embedded: the
    // GGUF files found in the models dir).
    virtual std::vector<std::string> list_models() = 0;

    // Throws SummarizerError on failure.
    virtual std::string summarize(const SummaryRequest& req,
                                  const ProgressFn& progress) = 0;

    // Release GPU/host memory. No-op for the remote backend.
    virtual void unload() {}

    virtual void request_abort() {}
};

// Picks the backend named by settings.llm_backend.
std::unique_ptr<Backend> make_backend(const Settings& settings,
                                      const DeviceInfo& device);

// Builds the user message: the context preamble (when present) followed by the
// transcript, under their section labels.
std::string build_user_message(const SummaryRequest& req);

// The effective system prompt: override if set, else the template's.
std::string resolve_system_prompt(const SummaryRequest& req);

}  // namespace transcriptor::llm
