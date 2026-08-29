// Persistent user settings, stored as JSON next to the app's config dir.
// Field names match the JSON the web UI sends, so no translation layer sits
// between the browser and this struct.
#pragma once

#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "util/paths.h"

namespace transcriptor {

// Per-template user edits, keyed by template id ("meeting", "standup", ...).
struct TemplateOverride {
    std::string prompt;    // replaces the built-in system prompt when non-empty
    std::string context;   // persistent extra context, prepended to every summary
};

// A template the user wrote, sitting in the menus next to the built-ins. Keyed
// by a generated id the UI mints once ("custom-1724930000000"), so renaming the
// label never breaks a saved summary_template.
struct CustomTemplate {
    std::string label;     // display name, shown in both template menus
    std::string prompt;    // system prompt; the whole point, so never empty
    std::string context;   // persistent extra context, like TemplateOverride
};

struct Settings {
    // -- audio ------------------------------------------------------------
    std::string source_id;              // empty = default output loopback
    int         samplerate  = 16000;    // 16 kHz mono for whisper + diarization
    float       mic_gain    = 1.0f;
    float       system_gain = 1.0f;

    // -- STT (whisper.cpp) ------------------------------------------------
    std::string whisper_model = "large-v3";   // tiny/base/small/medium/large-v3
    std::string whisper_model_path;           // explicit .bin; empty = managed
    std::string language      = "tr";         // "" = auto-detect
    std::string device        = "auto";       // auto/cuda/cpu
    std::string compute_type  = "auto";       // kept for UI parity (quantization)
    int         stt_threads   = 0;            // 0 = hardware_concurrency

    // -- diarization (sherpa-onnx) ---------------------------------------
    bool        enable_diarization = true;
    std::string diar_segmentation_model;      // empty = managed download
    std::string diar_embedding_model;
    int         num_speakers      = 0;        // 0 = estimate from clustering
    float       cluster_threshold = 0.5f;

    // -- summarizer -------------------------------------------------------
    // "embedded" = llama.cpp in-process; "remote" = OpenAI-compatible server.
    std::string llm_backend    = "embedded";
    std::string llm_model_path;               // GGUF for the embedded backend
    int         llm_ctx        = 8192;
    int         llm_gpu_layers = 999;         // 999 = offload everything it can
    int         llm_threads    = 0;
    int         llm_max_tokens = 2048;
    float       llm_temperature = 0.2f;

    std::string llm_base_url = "http://127.0.0.1:1234/v1";
    std::string llm_model;                    // "" = first model the server lists
    std::string llm_api_key  = "lm-studio";
    double      llm_timeout  = 600.0;

    std::string ui_language      = "tr";   // interface chrome: "tr" or "en"
    std::string summary_language = "tr";
    std::string summary_template = "meeting";
    std::map<std::string, TemplateOverride> template_overrides;
    std::map<std::string, CustomTemplate>   custom_templates;

    // -- output / export --------------------------------------------------
    std::string output_dir;                   // set in the constructor
    bool save_audio      = true;
    bool save_transcript = true;
    bool save_summary    = true;
    bool auto_summarize  = false;

    // Sequence VRAM: unload whisper before the LLM loads, and vice versa, so a
    // single 8 GB card never has to hold both at once.
    bool manage_vram = true;

    // -- local server -----------------------------------------------------
    std::string host = "127.0.0.1";
    int         port = 5005;

    Settings();

    static paths::fs::path config_path();

    // Reads config.json if present, then applies environment overrides.
    // Missing or malformed files fall back to defaults rather than failing.
    static Settings load();

    bool save() const;

    nlohmann::json to_json() const;
    void from_json(const nlohmann::json& j);

    // Resolved model file locations (managed download path when unset).
    paths::fs::path whisper_model_file() const;
    paths::fs::path segmentation_model_file() const;
    paths::fs::path embedding_model_file() const;

private:
    void apply_env();
};

}  // namespace transcriptor
