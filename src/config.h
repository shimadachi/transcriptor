// Persistent user settings, stored as JSON next to the app's config dir.
// Field names match the JSON the web UI sends, so no translation layer sits
// between the browser and this struct.
#pragma once

#include <functional>
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
    // Empty until the user picks one. There is deliberately no default: the
    // old one made the first recording quietly pull 3 GB down mid-transcribe.
    std::string whisper_model;                // a models::whisper_catalog() id
    std::string whisper_model_path;           // explicit .bin; empty = managed
    std::string language      = "en";         // "" = auto-detect
    std::string device        = "auto";       // auto/cuda/cpu
    std::string compute_type  = "auto";       // kept for UI parity (quantization)
    int         stt_threads   = 0;            // 0 = hardware_concurrency

    // -- diarization (sherpa-onnx) ---------------------------------------
    bool        enable_diarization = false;
    std::string diar_segmentation_model;      // empty = managed download
    std::string diar_embedding_model;
    int         num_speakers      = 0;        // 0 = estimate from clustering
    // There is deliberately no clustering-threshold setting. See
    // cluster_threshold(), which derives it from `language`.

    // -- summarizer -------------------------------------------------------
    // "embedded" = llama.cpp in-process; "remote" = OpenAI-compatible server.
    std::string llm_backend    = "embedded";
    std::string llm_model_path;               // GGUF for the embedded backend
    // 0 = size the window from the model's own trained context and the memory
    // left after its weights load. A number here overrides that, as typed.
    int         llm_ctx        = 0;
    int         llm_gpu_layers = 999;         // 999 = offload everything it can
    int         llm_threads    = 0;
    int         llm_max_tokens = 2048;
    float       llm_temperature = 0.2f;

    // Let a reasoning model think before the final summary. Off by default:
    // thinking costs tokens and time on every pass, and the section notes of a
    // long recording never benefit from it — so it is spent on the one pass
    // where weighing a whole meeting against itself actually pays, and it is
    // given a budget of its own rather than the answer's.
    bool        llm_thinking   = false;

    std::string llm_base_url = "http://127.0.0.1:1234/v1";
    std::string llm_model;                    // "" = first model the server lists
    std::string llm_api_key  = "lm-studio";
    double      llm_timeout  = 600.0;

    std::string ui_language      = "en";   // interface chrome: "en" or "tr"
    std::string ui_theme         = "system";  // "system" / "light" / "dark"
    std::string summary_language = "en";
    std::string summary_template = "meeting";
    std::map<std::string, TemplateOverride> template_overrides;
    std::map<std::string, CustomTemplate>   custom_templates;

    // -- output / export --------------------------------------------------
    std::string output_dir;                   // set in the constructor
    bool save_audio      = true;
    bool save_transcript = true;
    bool save_summary    = true;
    // Off = the recording waits in memory until the Transcribe button is
    // pressed; the same gate the summarizer has always had, and the same
    // default. Loading a Whisper model the moment a recording stops is a long,
    // loud, memory-hungry thing to start on its own.
    bool auto_transcribe = false;
    bool auto_summarize  = false;

    // Sequence VRAM: unload whisper before the LLM loads, and vice versa, so a
    // single 8 GB card never has to hold both at once.
    bool manage_vram = true;

    // -- updates ----------------------------------------------------------
    // Once a day the UI asks GitHub for the newest release tag and shows a
    // banner when it is newer than this build. It is the only request the app
    // makes on its own — every other transfer is something the user started —
    // so it gets its own switch. Off means the check never runs at all.
    bool check_updates = true;

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

    // The clustering threshold to hand sherpa, derived from `language`.
    float cluster_threshold() const;

    // Whether the transcriber and the summarizer would come out the same when
    // built from `other`. Most saves touch neither -- a theme, a template, an
    // output folder -- and rebuilding them regardless threw away loaded models.
    bool same_engines(const Settings& other) const;

    // Resolved model file locations (managed download path when unset).
    paths::fs::path whisper_model_file() const;
    paths::fs::path segmentation_model_file() const;
    paths::fs::path embedding_model_file() const;

    // Changes meant for this run only: the environment overrides load()
    // applies, a --port on the command line. save() writes back what the
    // file held for any setting they changed, unless it has been changed
    // again since -- so a one-off TRANSCRIPTOR_OUTPUT_DIR does not become the
    // output folder for good the next time anything at all is saved.
    void override_for_this_run(const std::function<void(Settings&)>& change);

private:
    void apply_env();

    // Keyed as in config.json: what the file held, and what the override set.
    struct RunOverride {
        nlohmann::json saved;
        nlohmann::json value;
    };
    std::map<std::string, RunOverride> run_overrides_;
};

}  // namespace transcriptor
