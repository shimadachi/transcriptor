#include "config.h"

#include <algorithm>
#include <cstdlib>

#include "util/lang.h"

namespace transcriptor {

namespace {

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

bool env_truthy(const std::string& v) {
    std::string s;
    for (char c : v) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

// Read a key only if present and of the expected type — an old or partial
// config file must never break startup.
template <typename T>
void get(const nlohmann::json& j, const char* key, T* out) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    try {
        *out = it->get<T>();
    } catch (const nlohmann::json::exception&) {
        // keep the default
    }
}

}  // namespace

Settings::Settings() {
    output_dir = paths::to_utf8(paths::default_output_dir());
}

paths::fs::path Settings::config_path() {
    return paths::config_dir() / "config.json";
}

nlohmann::json Settings::to_json() const {
    nlohmann::json ov = nlohmann::json::object();
    for (const auto& [key, o] : template_overrides) {
        ov[key] = {{"prompt", o.prompt}, {"context", o.context}};
    }
    nlohmann::json custom = nlohmann::json::object();
    for (const auto& [key, c] : custom_templates) {
        custom[key] = {{"label", c.label}, {"prompt", c.prompt},
                       {"context", c.context}};
    }
    return {
        {"source_id", source_id},
        {"samplerate", samplerate},
        {"mic_gain", mic_gain},
        {"system_gain", system_gain},

        {"whisper_model", whisper_model},
        {"whisper_model_path", whisper_model_path},
        {"language", language},
        {"device", device},
        {"compute_type", compute_type},
        {"stt_threads", stt_threads},

        {"enable_diarization", enable_diarization},
        {"diar_segmentation_model", diar_segmentation_model},
        {"diar_embedding_model", diar_embedding_model},
        {"num_speakers", num_speakers},
        {"cluster_threshold", cluster_threshold},

        {"llm_backend", llm_backend},
        {"llm_model_path", llm_model_path},
        {"llm_ctx", llm_ctx},
        {"llm_gpu_layers", llm_gpu_layers},
        {"llm_threads", llm_threads},
        {"llm_max_tokens", llm_max_tokens},
        {"llm_temperature", llm_temperature},
        {"llm_base_url", llm_base_url},
        {"llm_model", llm_model},
        {"llm_api_key", llm_api_key},
        {"llm_timeout", llm_timeout},

        {"ui_language", ui_language},
        {"ui_theme", ui_theme},
        {"summary_language", summary_language},
        {"summary_template", summary_template},
        {"template_overrides", ov},
        {"custom_templates", custom},

        {"output_dir", output_dir},
        {"save_audio", save_audio},
        {"save_transcript", save_transcript},
        {"save_summary", save_summary},
        {"auto_transcribe", auto_transcribe},
        {"auto_summarize", auto_summarize},
        {"manage_vram", manage_vram},
        {"check_updates", check_updates},

        {"host", host},
        {"port", port},
    };
}

void Settings::from_json(const nlohmann::json& j) {
    if (!j.is_object()) return;

    get(j, "source_id", &source_id);
    get(j, "samplerate", &samplerate);
    get(j, "mic_gain", &mic_gain);
    get(j, "system_gain", &system_gain);

    get(j, "whisper_model", &whisper_model);
    get(j, "whisper_model_path", &whisper_model_path);
    get(j, "language", &language);
    get(j, "device", &device);
    get(j, "compute_type", &compute_type);
    get(j, "stt_threads", &stt_threads);

    get(j, "enable_diarization", &enable_diarization);
    get(j, "diar_segmentation_model", &diar_segmentation_model);
    get(j, "diar_embedding_model", &diar_embedding_model);
    get(j, "num_speakers", &num_speakers);
    get(j, "cluster_threshold", &cluster_threshold);

    get(j, "llm_backend", &llm_backend);
    get(j, "llm_model_path", &llm_model_path);
    get(j, "llm_ctx", &llm_ctx);
    get(j, "llm_gpu_layers", &llm_gpu_layers);
    get(j, "llm_threads", &llm_threads);
    get(j, "llm_max_tokens", &llm_max_tokens);
    get(j, "llm_temperature", &llm_temperature);
    get(j, "llm_base_url", &llm_base_url);
    get(j, "llm_model", &llm_model);
    get(j, "llm_api_key", &llm_api_key);
    get(j, "llm_timeout", &llm_timeout);

    get(j, "ui_language", &ui_language);
    if (ui_language != "tr") ui_language = "en";
    get(j, "ui_theme", &ui_theme);
    if (ui_theme != "light" && ui_theme != "dark") ui_theme = "system";
    get(j, "summary_language", &summary_language);
    get(j, "summary_template", &summary_template);

    auto it = j.find("template_overrides");
    if (it != j.end() && it->is_object()) {
        template_overrides.clear();
        for (auto& [key, v] : it->items()) {
            if (!v.is_object()) continue;
            TemplateOverride o;
            get(v, "prompt", &o.prompt);
            get(v, "context", &o.context);
            if (!o.prompt.empty() || !o.context.empty()) template_overrides[key] = o;
        }
    }

    auto custom = j.find("custom_templates");
    if (custom != j.end() && custom->is_object()) {
        custom_templates.clear();
        for (auto& [key, v] : custom->items()) {
            if (!v.is_object()) continue;
            CustomTemplate c;
            get(v, "label", &c.label);
            get(v, "prompt", &c.prompt);
            get(v, "context", &c.context);
            // A template with no prompt has nothing to say; drop it.
            if (c.prompt.empty()) continue;
            if (c.label.empty()) c.label = key;
            custom_templates[key] = c;
        }
    }

    get(j, "output_dir", &output_dir);
    get(j, "save_audio", &save_audio);
    get(j, "save_transcript", &save_transcript);
    get(j, "save_summary", &save_summary);
    get(j, "auto_transcribe", &auto_transcribe);
    get(j, "auto_summarize", &auto_summarize);
    get(j, "manage_vram", &manage_vram);
    get(j, "check_updates", &check_updates);

    get(j, "host", &host);
    get(j, "port", &port);
}

Settings Settings::load() {
    Settings s;
    std::string raw;
    if (paths::read_file(config_path(), &raw)) {
        auto j = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
        if (!j.is_discarded()) s.from_json(j);
    }
    s.apply_env();
    // Everything downstream — decoders, downloads, the summarizer — reads the
    // language from here rather than being handed a Settings.
    lang::set(s.ui_language);
    return s;
}

bool Settings::save() const {
    return paths::write_file(config_path(), to_json().dump(2));
}

void Settings::apply_env() {
    if (const char* v = env("TRANSCRIPTOR_HOST")) host = v;
    if (const char* v = env("TRANSCRIPTOR_PORT")) {
        char* end = nullptr;
        long p = std::strtol(v, &end, 10);
        if (end != v && p > 0 && p < 65536) port = static_cast<int>(p);
    }
    if (const char* v = env("TRANSCRIPTOR_LLM_BASE_URL")) llm_base_url = v;
    if (const char* v = env("TRANSCRIPTOR_LLM_MODEL")) llm_model = v;
    if (const char* v = env("TRANSCRIPTOR_LLM_MODEL_PATH")) llm_model_path = v;
    if (const char* v = env("TRANSCRIPTOR_OUTPUT_DIR")) output_dir = v;
    if (const char* v = env("TRANSCRIPTOR_WHISPER_MODEL")) whisper_model = v;
    if (const char* v = env("TRANSCRIPTOR_DEVICE")) device = v;
    if (const char* v = env("TRANSCRIPTOR_NO_DIARIZE")) {
        if (env_truthy(v)) enable_diarization = false;
    }
}

paths::fs::path Settings::whisper_model_file() const {
    if (!whisper_model_path.empty()) return paths::expand_user(whisper_model_path);
    return paths::models_dir() / ("ggml-" + whisper_model + ".bin");
}

paths::fs::path Settings::segmentation_model_file() const {
    if (!diar_segmentation_model.empty())
        return paths::expand_user(diar_segmentation_model);
    return paths::models_dir() / "diarize" / "segmentation.onnx";
}

paths::fs::path Settings::embedding_model_file() const {
    if (!diar_embedding_model.empty())
        return paths::expand_user(diar_embedding_model);
    return paths::models_dir() / "diarize" / "speaker-embedding.onnx";
}

}  // namespace transcriptor
