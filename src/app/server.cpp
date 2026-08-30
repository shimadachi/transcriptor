#include "app/server.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "app/assets.h"
#include "audio/sources.h"
#include "diarize/diarizer.h"
#include "llm/llama_backend.h"
#include "llm/templates.h"
#include "util/export.h"
#include "util/lang.h"
#include "util/library.h"
#include "util/models.h"

namespace transcriptor::app {

namespace {

using json = nlohmann::json;

constexpr size_t kMaxUpload = 4ull * 1024 * 1024 * 1024;   // 4 GB audio/video

void send_json(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

void send_error(httplib::Response& res, const std::string& message,
                int status = 400) {
    send_json(res, json{{"error", message}}, status);
}

json parse_body(const httplib::Request& req) {
    if (req.body.empty()) return json::object();
    json j = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    return j.is_object() ? j : json::object();
}

std::string get_string(const json& j, const char* key, const std::string& fallback = "") {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Strip directory components and anything hostile from an uploaded filename.
std::string safe_filename(const std::string& name) {
    std::string base = name;
    const auto slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);

    std::string out;
    for (char c : base) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
                        c == '-' || c == '_' || c == ' ';
        out += ok ? c : '_';
    }
    out = trim(out);
    return out.empty() ? "audio" : out;
}

// Custom template ids are minted by the UI and land in config.json and in URLs
// of nothing else, but keep them boring anyway: no separators, no surprises.
bool valid_template_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
                        c == '_';
        if (!ok) return false;
    }
    return !llm::is_template(id);   // never shadow a built-in
}

json source_json(const audio::AudioSource& s) {
    return {{"id", s.id}, {"label", s.label()}, {"is_loopback", s.is_loopback}};
}

}  // namespace

struct Server::Impl {
    httplib::Server svr;
    std::thread     thread;
    std::atomic<bool> running{false};
};

Server::Server(AppState* state, std::string host, int port)
    : state_(state), host_(std::move(host)), port_(port),
      impl_(std::make_unique<Impl>()) {}

Server::~Server() { stop(); }

std::string Server::base_url() const {
    return "http://" + host_ + ":" + std::to_string(port_);
}

bool Server::start() {
    httplib::Server& svr = impl_->svr;
    AppState* state = state_;

    svr.set_payload_max_length(kMaxUpload);

    // -- static ------------------------------------------------------------
    auto serve_asset = [](const std::string& path, httplib::Response& res) {
        const Asset* asset = find_asset(path);
        if (!asset) {
            res.status = 404;
            res.set_content("not found", "text/plain");
            return;
        }
        res.set_content(asset->data, asset->size, asset->content_type);
    };

    svr.Get("/", [serve_asset](const httplib::Request&, httplib::Response& res) {
        serve_asset("index.html", res);
    });

    svr.Get(R"(/static/(.+))", [serve_asset](const httplib::Request& req,
                                             httplib::Response& res) {
        serve_asset(req.matches[1].str(), res);
    });

    // -- state -------------------------------------------------------------
    svr.Get("/api/state", [state](const httplib::Request&, httplib::Response& res) {
        send_json(res, state->state_json());
    });

    svr.Get("/api/result", [state](const httplib::Request&, httplib::Response& res) {
        send_json(res, state->result_json());
    });

    // -- audio sources -----------------------------------------------------
    svr.Get("/api/sources", [](const httplib::Request&, httplib::Response& res) {
        json body;
        try {
            json arr = json::array();
            for (const auto& s : audio::list_sources()) arr.push_back(source_json(s));
            body["sources"] = arr;
        } catch (const std::exception& e) {
            // 200 with an error field: the UI shows it inline, like Flask did.
            send_json(res, json{{"error", e.what()}, {"sources", json::array()}});
            return;
        }
        const auto def = audio::default_loopback_source();
        body["default_id"] = def ? json(def->id) : json(nullptr);

        const std::string hint = audio::macos_loopback_hint();
        if (!hint.empty()) body["hint"] = hint;

        send_json(res, body);
    });

    // -- recording ---------------------------------------------------------
    svr.Post("/api/record/start", [state](const httplib::Request& req,
                                          httplib::Response& res) {
        if (state->recording()) return send_error(res, L("Already recording.", "Zaten kayıtta."));
        if (state->processing()) {
            return send_error(res, L("A job is already running.", "İşlem sürüyor."));
        }

        const json body = parse_body(req);

        // Fall back to the default loopback when the id is missing or stale.
        auto source = audio::find_source(get_string(body, "source_id"));
        if (!source) source = audio::default_loopback_source();
        if (!source) {
            std::string msg = L("No usable audio source.", "Geçerli ses kaynağı yok.");
            const std::string hint = audio::macos_loopback_hint();
            if (!hint.empty()) msg += " " + hint;
            return send_error(res, msg);
        }

        // Optional second source (a mic) mixed into the system audio. Exact
        // match only — no default fallback.
        std::optional<audio::AudioSource> mic;
        const std::string mic_id = get_string(body, "mic_source_id");
        if (!mic_id.empty()) {
            mic = audio::find_source(mic_id);
            if (!mic) {
                return send_error(res, L("The selected microphone was not found.",
                                         "Seçilen mikrofon bulunamadı."));
            }
        }

        try {
            state->start_recording(*source, mic);
        } catch (const std::exception& e) {
            return send_error(res, e.what(), 500);
        }
        send_json(res, json{{"ok", true}});
    });

    svr.Post("/api/record/stop", [state](const httplib::Request&,
                                         httplib::Response& res) {
        if (!state->recording()) return send_error(res, L("Nothing is recording.", "Kayıt yok."));
        state->stop_and_process();
        send_json(res, json{{"ok", true}});
    });

    svr.Post("/api/record/pause", [state](const httplib::Request&,
                                          httplib::Response& res) {
        if (!state->recording()) return send_error(res, L("Nothing is recording.", "Kayıt yok."));
        state->pause_recording();
        send_json(res, json{{"ok", true}, {"paused", true}});
    });

    svr.Post("/api/record/resume", [state](const httplib::Request&,
                                           httplib::Response& res) {
        if (!state->recording()) return send_error(res, L("Nothing is recording.", "Kayıt yok."));
        state->resume_recording();
        send_json(res, json{{"ok", true}, {"paused", false}});
    });

    svr.Post("/api/cancel", [state](const httplib::Request&,
                                    httplib::Response& res) {
        if (state->processing()) {
            return send_error(res, L("A job is running; cancel once it finishes.",
                                     "İşlem sürüyor; bitince iptal edin."));
        }
        state->cancel();
        send_json(res, json{{"ok", true}});
    });

    // -- process an existing file -----------------------------------------
    svr.Post("/api/process_file", [state](const httplib::Request& req,
                                          httplib::Response& res) {
        if (state->recording() || state->processing()) {
            return send_error(res, L("A job is already running.", "İşlem sürüyor."));
        }
        if (!req.has_file("file")) {
            return send_error(res, L("No file selected.", "Dosya seçilmedi."));
        }

        const httplib::MultipartFormData& file = req.get_file_value("file");
        if (file.filename.empty() || file.content.empty()) {
            return send_error(res, L("No file selected.", "Dosya seçilmedi."));
        }

        const std::string name = safe_filename(file.filename);
        const paths::fs::path tmp =
            paths::fs::temp_directory_path() /
            paths::from_utf8("transcriptor_upload_" + std::to_string(std::rand()) + "_" + name);

        if (!paths::write_file(tmp, file.content)) {
            return send_error(res,
                              L("The upload could not be written to the temp folder.",
                                "Yüklenen dosya geçici klasöre yazılamadı."), 500);
        }

        const bool ok = state->process_file(tmp, name);

        std::error_code ec;
        paths::fs::remove(tmp, ec);

        if (!ok) return send_error(res, state->message());
        send_json(res, json{{"ok", true}});
    });

    svr.Post("/api/open_folder", [state](const httplib::Request&,
                                         httplib::Response& res) {
        const paths::fs::path dir = state->session_dir();
        if (dir.empty()) {
            return send_error(res, L("Nothing has been saved yet.",
                                     "Henüz kaydedilmiş çıktı yok."));
        }
        const bool ok = exporter::open_in_file_manager(dir);
        send_json(res, json{{"ok", ok}, {"path", paths::to_utf8(dir)}});
    });

    // -- library (past sessions in the output folder) -----------------------
    // The folder is the only store: nothing is indexed, so a session copied in
    // by hand shows up and one deleted outside the app quietly disappears.
    svr.Get("/api/library", [state](const httplib::Request&,
                                    httplib::Response& res) {
        const Settings s = state->settings_copy();

        json arr = json::array();
        for (const library::Entry& e : library::list(s.output_dir)) {
            arr.push_back({
                {"id", e.id},
                {"path", e.path},
                {"mtime", e.mtime},
                {"has_transcript", e.has_transcript},
                {"has_summary", e.has_summary},
                {"audio", e.audio.empty() ? json(nullptr) : json(e.audio)},
                {"audio_bytes", e.audio_bytes},
                {"preview", e.preview},
            });
        }
        send_json(res, json{
            {"output_dir", paths::to_utf8(paths::expand_user(s.output_dir))},
            {"sessions", arr},
        });
    });

    svr.Get("/api/library/item", [state](const httplib::Request& req,
                                         httplib::Response& res) {
        const Settings s = state->settings_copy();
        const paths::fs::path dir =
            library::resolve(s.output_dir, req.get_param_value("id"));
        if (dir.empty()) {
            return send_error(res, L("That recording is no longer there.",
                                     "Bu kayıt artık yerinde değil."), 404);
        }

        json body;
        body["id"]   = paths::to_utf8(dir.filename());
        body["path"] = paths::to_utf8(dir);

        std::string raw;
        // transcript.json carries speakers and timestamps, so the library shows
        // it exactly the way the live transcript panel does; the .txt is only
        // the fallback for a session saved before the JSON existed.
        body["transcript"] = json(nullptr);
        if (paths::read_file(dir / "transcript.json", &raw)) {
            json parsed = json::parse(raw, nullptr, /*allow_exceptions=*/false);
            if (!parsed.is_discarded()) body["transcript"] = parsed;
        }
        body["transcript_text"] =
            paths::read_file(dir / "transcript.txt", &raw) ? json(raw) : json(nullptr);
        body["summary"] =
            paths::read_file(dir / "summary.txt", &raw) ? json(raw) : json(nullptr);

        const std::string audio = library::find_audio(dir);
        body["audio"] = audio.empty() ? json(nullptr) : json(audio);
        send_json(res, body);
    });

    // Streamed rather than buffered: an hour of WAV is ~110 MB, and <audio>
    // wants byte ranges to seek.
    svr.Get("/api/library/audio", [state](const httplib::Request& req,
                                          httplib::Response& res) {
        const Settings s = state->settings_copy();
        const paths::fs::path dir =
            library::resolve(s.output_dir, req.get_param_value("id"));
        if (dir.empty()) {
            return send_error(res, L("That recording is no longer there.",
                                     "Bu kayıt artık yerinde değil."), 404);
        }

        const std::string name = library::find_audio(dir);
        if (name.empty()) {
            return send_error(res, L("This recording has no audio file.",
                                     "Bu kaydın ses dosyası yok."), 404);
        }

        const paths::fs::path file = dir / paths::from_utf8(name);
        std::error_code ec;
        const auto size = paths::fs::file_size(file, ec);
        if (ec) {
            return send_error(res, L("The audio file could not be read.",
                                     "Ses dosyası okunamadı."), 404);
        }

        auto in = std::make_shared<std::ifstream>(file, std::ios::binary);
        if (!*in) {
            return send_error(res, L("The audio file could not be read.",
                                     "Ses dosyası okunamadı."), 500);
        }

        res.set_header("Accept-Ranges", "bytes");
        res.set_content_provider(
            static_cast<size_t>(size), library::content_type_for(name),
            [in](size_t offset, size_t length, httplib::DataSink& sink) -> bool {
                constexpr size_t kChunk = 256 * 1024;
                std::vector<char> buf(std::min<size_t>(length, kChunk));
                in->clear();   // a previous range may have hit EOF
                in->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                in->read(buf.data(), static_cast<std::streamsize>(buf.size()));
                const auto n = in->gcount();
                if (n <= 0) return false;
                return sink.write(buf.data(), static_cast<size_t>(n));
            });
    });

    svr.Post("/api/library/open", [state](const httplib::Request& req,
                                          httplib::Response& res) {
        const Settings s = state->settings_copy();
        const paths::fs::path dir =
            library::resolve(s.output_dir, get_string(parse_body(req), "id"));
        if (dir.empty()) {
            return send_error(res, L("That recording is no longer there.",
                                     "Bu kayıt artık yerinde değil."), 404);
        }
        const bool ok = exporter::open_in_file_manager(dir);
        send_json(res, json{{"ok", ok}, {"path", paths::to_utf8(dir)}});
    });

    // -- summarize ---------------------------------------------------------
    svr.Post("/api/summarize", [state](const httplib::Request& req,
                                       httplib::Response& res) {
        if (state->processing()) {
            return send_error(res, L("A job is already running.", "İşlem sürüyor."));
        }

        const json body = parse_body(req);
        const Settings settings = state->settings_copy();
        const bool tr = (settings.summary_language == "tr");

        // Build the context preamble from the structured fields.
        std::vector<std::string> parts;
        const std::string title  = trim(get_string(body, "title"));
        const std::string people = trim(get_string(body, "participants"));
        const std::string notes  = trim(get_string(body, "notes"));
        if (!title.empty())  parts.push_back((tr ? "Başlık: " : "Title: ") + title);
        if (!people.empty()) parts.push_back((tr ? "Katılımcılar: " : "Participants: ") + people);
        if (!notes.empty())  parts.push_back((tr ? "Notlar: " : "Notes: ") + notes);

        std::string context;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) context += "\n";
            context += parts[i];
        }

        state->start_summarize(context, trim(get_string(body, "template")));
        send_json(res, json{{"ok", true}});
    });

    // -- settings ----------------------------------------------------------
    svr.Get("/api/settings", [state](const httplib::Request&,
                                     httplib::Response& res) {
        const Settings s = state->settings_copy();

        // Built-ins first, then the user's own, so the menu order stays stable.
        // Menu labels follow the interface language; the prompts below follow
        // summary_language, since that is the language they are written in.
        json templates = json::array();
        for (const auto& t : llm::template_labels(s.ui_language)) {
            templates.push_back({{"value", t.value}, {"label", t.label},
                                 {"custom", false}});
        }
        for (const auto& [key, c] : s.custom_templates) {
            templates.push_back({{"value", key}, {"label", c.label},
                                 {"custom", true}});
        }

        json defaults = json::object();
        for (const std::string& id : llm::template_ids()) {
            defaults[id] = llm::system_prompt(id, s.summary_language);
        }

        json overrides = json::object();
        for (const auto& [key, o] : s.template_overrides) {
            overrides[key] = {{"prompt", o.prompt}, {"context", o.context}};
        }

        json customs = json::object();
        for (const auto& [key, c] : s.custom_templates) {
            customs[key] = {{"label", c.label}, {"prompt", c.prompt},
                            {"context", c.context}};
        }

        json gguf = json::array();
        for (const std::string& p : llm::discover_gguf_models()) gguf.push_back(p);

        // Downloadable summarizer models, with what is already on disk marked.
        json catalog = json::array();
        for (const models::LlmModelSpec& m : models::llm_catalog()) {
            const paths::fs::path file = models::llm_model_file(m);
            std::error_code ec;
            catalog.push_back({{"id", m.id},
                               {"label", m.label},
                               {"note", m.note()},
                               {"size", models::human_size(m.approx_bytes)},
                               {"path", paths::to_utf8(file)},
                               {"downloaded", paths::fs::exists(file, ec)}});
        }

        send_json(res, json{
            {"whisper_model", s.whisper_model},
            {"whisper_model_path", s.whisper_model_path},
            {"language", s.language},
            {"device", s.device},
            {"compute_type", s.compute_type},

            {"enable_diarization", s.enable_diarization},
            {"diar_supported", diarize::Diarizer::supported()},
            {"diar_segmentation_model", s.diar_segmentation_model},
            {"diar_embedding_model", s.diar_embedding_model},
            {"num_speakers", s.num_speakers},
            {"cluster_threshold", s.cluster_threshold},

            {"llm_backend", s.llm_backend},
            {"llm_model_path", s.llm_model_path},
            {"llm_ctx", s.llm_ctx},
            {"llm_gpu_layers", s.llm_gpu_layers},
            {"llm_max_tokens", s.llm_max_tokens},
            {"gguf_models", gguf},
            {"llm_catalog", catalog},
            {"llm_download", state->llm_download_json()},
            {"llm_base_url", s.llm_base_url},
            {"llm_model", s.llm_model},
            {"llm_timeout", s.llm_timeout},

            {"ui_language", s.ui_language},
            {"ui_theme", s.ui_theme},
            {"summary_language", s.summary_language},
            {"summary_template", s.summary_template},
            {"templates", templates},
            {"template_defaults", defaults},
            {"template_overrides", overrides},
            {"custom_templates", customs},

            {"output_dir", s.output_dir},
            {"models_dir", paths::to_utf8(paths::models_dir())},
            {"save_audio", s.save_audio},
            {"save_transcript", s.save_transcript},
            {"save_summary", s.save_summary},
            {"auto_summarize", s.auto_summarize},
            {"manage_vram", s.manage_vram},
            {"mic_gain", s.mic_gain},
            {"system_gain", s.system_gain},
        });
    });

    svr.Post("/api/settings", [state](const httplib::Request& req,
                                      httplib::Response& res) {
        if (state->processing() || state->recording()) {
            return send_error(res,
                              L("A job is running; save the settings once it finishes.",
                                "İşlem sürüyor; bitince ayarları kaydedin."));
        }

        const json body = parse_body(req);
        Settings s = state->settings_copy();

        auto str = [&body](const char* key, std::string* out) {
            auto it = body.find(key);
            if (it != body.end() && it->is_string()) *out = it->get<std::string>();
        };
        auto flag = [&body](const char* key, bool* out) {
            auto it = body.find(key);
            if (it != body.end() && it->is_boolean()) *out = it->get<bool>();
        };
        auto clamped_float = [&body](const char* key, float* out, float lo, float hi) {
            auto it = body.find(key);
            if (it != body.end() && it->is_number()) {
                *out = std::clamp(it->get<float>(), lo, hi);
            }
        };
        auto clamped_int = [&body](const char* key, int* out, int lo, int hi) {
            auto it = body.find(key);
            if (it != body.end() && it->is_number()) {
                *out = std::clamp(it->get<int>(), lo, hi);
            }
        };

        str("whisper_model", &s.whisper_model);
        str("whisper_model_path", &s.whisper_model_path);
        str("language", &s.language);
        str("device", &s.device);
        str("compute_type", &s.compute_type);
        str("diar_segmentation_model", &s.diar_segmentation_model);
        str("diar_embedding_model", &s.diar_embedding_model);
        str("llm_backend", &s.llm_backend);
        str("llm_model_path", &s.llm_model_path);
        str("llm_base_url", &s.llm_base_url);
        str("llm_model", &s.llm_model);
        str("ui_language", &s.ui_language);
        if (s.ui_language != "tr") s.ui_language = "en";
        str("ui_theme", &s.ui_theme);
        if (s.ui_theme != "light" && s.ui_theme != "dark") s.ui_theme = "system";
        str("summary_language", &s.summary_language);
        str("summary_template", &s.summary_template);
        str("output_dir", &s.output_dir);

        flag("enable_diarization", &s.enable_diarization);
        flag("save_audio", &s.save_audio);
        flag("save_transcript", &s.save_transcript);
        flag("save_summary", &s.save_summary);
        flag("auto_summarize", &s.auto_summarize);
        flag("manage_vram", &s.manage_vram);

        clamped_float("mic_gain", &s.mic_gain, 0.0f, 4.0f);
        clamped_float("system_gain", &s.system_gain, 0.0f, 4.0f);
        clamped_float("cluster_threshold", &s.cluster_threshold, 0.05f, 0.95f);
        clamped_int("num_speakers", &s.num_speakers, 0, 20);
        clamped_int("llm_ctx", &s.llm_ctx, 1024, 131072);
        clamped_int("llm_gpu_layers", &s.llm_gpu_layers, 0, 999);
        clamped_int("llm_max_tokens", &s.llm_max_tokens, 64, 16384);

        auto timeout = body.find("llm_timeout");
        if (timeout != body.end() && timeout->is_number()) {
            s.llm_timeout = std::clamp(timeout->get<double>(), 10.0, 3600.0);
        }

        if (s.llm_backend != "remote") s.llm_backend = "embedded";

        // Parsed before summary_template is validated, so a template created in
        // this same save can be the one selected.
        auto customs = body.find("custom_templates");
        if (customs != body.end() && customs->is_object()) {
            constexpr std::size_t kMaxCustom = 50;
            std::map<std::string, CustomTemplate> clean;
            for (const auto& [key, v] : customs->items()) {
                if (clean.size() >= kMaxCustom) break;
                if (!valid_template_id(key) || !v.is_object()) continue;
                CustomTemplate c;
                c.label   = trim(get_string(v, "label"));
                c.prompt  = trim(get_string(v, "prompt"));
                c.context = trim(get_string(v, "context"));
                if (c.prompt.empty()) continue;   // nothing to instruct with
                if (c.label.empty()) c.label = key;
                clean[key] = std::move(c);
            }
            s.custom_templates = std::move(clean);
        }

        if (!llm::is_template(s.summary_template) &&
            !s.custom_templates.count(s.summary_template)) {
            s.summary_template = "meeting";
        }

        auto overrides = body.find("template_overrides");
        if (overrides != body.end() && overrides->is_object()) {
            // Keep only known templates and the two editable string fields.
            std::map<std::string, TemplateOverride> clean;
            for (const auto& [key, v] : overrides->items()) {
                if (!llm::is_template(key) || !v.is_object()) continue;
                TemplateOverride o;
                o.prompt  = trim(get_string(v, "prompt"));
                o.context = trim(get_string(v, "context"));
                if (!o.prompt.empty() || !o.context.empty()) clean[key] = o;
            }
            s.template_overrides = std::move(clean);
        }

        s.save();
        state->replace_settings(s);

        const json snapshot = state->state_json();
        send_json(res, json{{"ok", true}, {"device", snapshot["device"]}});
    });

    // -- LLM model discovery ----------------------------------------------
    svr.Post("/api/llm/models", [state](const httplib::Request& req,
                                        httplib::Response& res) {
        const json body = parse_body(req);
        try {
            auto models = state->list_llm_models(get_string(body, "llm_base_url"));
            send_json(res, json{{"models", models}});
        } catch (const std::exception& e) {
            // 200 with an error field, matching the Flask behaviour the UI expects.
            send_json(res, json{{"error", e.what()}, {"models", json::array()}});
        }
    });

    // -- summarizer model download ----------------------------------------
    svr.Post("/api/llm/download", [state](const httplib::Request& req,
                                          httplib::Response& res) {
        const json body = parse_body(req);
        const std::string id = trim(get_string(body, "id"));
        if (id.empty()) return send_error(res, L("No model selected.", "Model seçilmedi."));

        std::string error;
        if (!state->start_llm_download(id, &error)) {
            return send_error(res, error, 409);
        }
        send_json(res, json{{"ok", true}, {"download", state->llm_download_json()}});
    });

    svr.Get("/api/llm/download", [state](const httplib::Request&,
                                         httplib::Response& res) {
        send_json(res, state->llm_download_json());
    });

    // -- bind & serve ------------------------------------------------------
    // Prefer the configured port so bookmarks keep working; fall back to any
    // free port when it is already taken (a second instance, say).
    if (port_ > 0 && svr.bind_to_port(host_.c_str(), port_)) {
        // bound to the requested port
    } else {
        const int bound = svr.bind_to_any_port(host_.c_str());
        if (bound <= 0) return false;
        port_ = bound;
    }

    impl_->running.store(true);
    impl_->thread = std::thread([this] {
        impl_->svr.listen_after_bind();
        impl_->running.store(false);
    });

    // Wait briefly for listen() to take effect so callers can open the URL.
    for (int i = 0; i < 100 && !svr.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

void Server::stop() {
    if (!impl_) return;
    impl_->svr.stop();
    if (impl_->thread.joinable()) impl_->thread.join();
}

}  // namespace transcriptor::app
