// OpenAI-compatible chat client for local servers: LM Studio (default),
// Ollama's /v1 endpoint, llama-server. An existing LM Studio setup works
// alongside the embedded backend.
#pragma once

#include <memory>

#include "llm/summarizer.h"

namespace transcriptor::llm {

std::unique_ptr<Backend> make_openai_backend(const Settings& settings);

}  // namespace transcriptor::llm
