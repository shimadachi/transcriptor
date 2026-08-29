// Embedded llama.cpp: loads a GGUF directly into this process.
//
// This is the compiled stand-in for LM Studio — no server to install, no HTTP
// hop, and the app can free the weights the moment a summary is done so the
// transcription model gets the GPU back.
#pragma once

#include <memory>

#include "llm/summarizer.h"

namespace transcriptor::llm {

std::unique_ptr<Backend> make_llama_backend(const Settings& settings,
                                            const DeviceInfo& device);

// GGUF files discovered in the models dir, for the settings dropdown.
std::vector<std::string> discover_gguf_models();

}  // namespace transcriptor::llm
