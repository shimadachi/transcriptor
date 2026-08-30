// Where the weights come from, and first-run downloads.
//
// Downloads are explicit: one known URL per model, fetched into the app's
// models dir. Users who prefer to fetch by hand can point Settings at any path
// and nothing is downloaded.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "config.h"
#include "util/paths.h"

namespace transcriptor::models {

struct ModelSpec {
    std::string   id;
    std::string   url;
    std::uint64_t approx_bytes = 0;
    std::string   label;          // shown in the progress message
};

// message is a ready-to-display status line; fraction is [0,1] or -1 (unknown).
using ProgressFn = std::function<void(const std::string& message, double fraction)>;

// Whisper GGML weights for "tiny".."large-v3"; empty url for unknown names.
ModelSpec whisper_spec(const std::string& model_name);

ModelSpec segmentation_spec();
ModelSpec embedding_spec();

// -- summarizer (GGUF) ----------------------------------------------------
// Small instruct models the settings panel offers for download. Nothing here
// ships inside the binary; the user picks one and it is fetched into the
// models dir on demand.
struct LlmModelSpec {
    std::string   id;             // "qwen3.5-4b"
    std::string   label;          // shown in the dropdown
    std::string   filename;       // file name inside the models dir
    std::string   url;
    std::uint64_t approx_bytes = 0;

    // One-line hint (size / hardware). Both languages are carried rather than
    // resolved here: the catalog is a function-local static, built once, so a
    // note picked at construction would freeze the language of the first call.
    std::string note_en;
    std::string note_tr;

    std::string note() const;     // the one matching the interface language
};

const std::vector<LlmModelSpec>& llm_catalog();

// nullptr when the id is not in the catalog.
const LlmModelSpec* llm_spec(const std::string& id);

// Where a catalog model lands: models_dir() / spec.filename.
paths::fs::path llm_model_file(const LlmModelSpec& spec);

// Empty string on success, a human-readable error otherwise. Returns
// immediately when the file is already on disk.
std::string ensure_llm_model(const LlmModelSpec& spec, const ProgressFn& progress);

// Each returns an empty string on success, or a human-readable error.
// If the file already exists, they return immediately without touching the
// network.
std::string ensure_whisper_model(const Settings& s, const ProgressFn& progress);
std::string ensure_diarization_models(const Settings& s, const ProgressFn& progress);

// True when every model the current settings need is already on disk.
bool whisper_ready(const Settings& s);
bool diarization_ready(const Settings& s);

std::string human_size(std::uint64_t bytes);

}  // namespace transcriptor::models
