#include "app/server.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "app/assets.h"
#include "app/shell.h"
#include "audio/sources.h"
#include "diarize/diarizer.h"
#include "llm/llama_backend.h"
#include "llm/summarizer.h"
#include "llm/templates.h"
#include "util/export.h"
#include "util/lang.h"
#include "util/library.h"
#include "util/models.h"

namespace transcriptor::app {

namespace {

using json = nlohmann::json;

constexpr size_t kMaxUpload = 4ull * 1024 * 1024 * 1024;   // 4 GB audio/video

// Where updates come from. The UI builds the releases API URL from the slug;
// /api/open_releases hands the page below to the browser. Both live here so
// the two never drift apart.
constexpr const char* kRepoSlug = "shimadachi/transcriptor";
constexpr const char* kReleasesUrl =
    "https://github.com/shimadachi/transcriptor/releases/latest";

void send_json(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    // Replace, don't throw. Text reaches these bodies from whisper, from the
    // summarizer, from other programs' error output and from file names on
    // disk, and none of it is guaranteed to be UTF-8. The strict default threw
    // on the first bad byte, so a single one in the status line answered every
    // /api/state with a 500 until something else changed the phase -- and the
    // page, unable to read the state, froze on whatever it last showed.
    res.set_content(body.dump(-1, ' ', false, json::error_handler_t::replace),
                    "application/json; charset=utf-8");
}

void send_error(httplib::Response& res, const std::string& message,
                int status = 400) {
    send_json(res, json{{"error", message}}, status);
}

json parse_body(const httplib::Request& req) {
    if (req.body.empty()) return json::object();
    // JSON bodies only. Parsing whatever arrived regardless of Content-Type was
    // half of what let a foreign page post one: text/plain keeps a cross-origin
    // POST "simple", so the browser sends it with no preflight to fail.
    const std::string type = req.get_header_value("Content-Type");
    if (type.rfind("application/json", 0) != 0) return json::object();
    json j = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    return j.is_object() ? j : json::object();
}

// The header the page proves itself with, and the tag it reads it out of.
constexpr const char* kTokenHeader = "X-Transcriptor-Token";
constexpr const char* kTokenPlaceholder = "__CSRF_TOKEN__";

// A fresh secret per run. Never persisted: a page from a previous run has no
// business driving this one, and reloading it costs nothing.
std::string random_token() {
    static const char kDigits[] = "0123456789abcdef";
    std::random_device rd;
    std::uniform_int_distribution<int> pick(0, 15);
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 32; ++i) out += kDigits[pick(rd)];
    return out;
}

// "http://127.0.0.1:5005" -> host "127.0.0.1". The port is deliberately not
// checked, so reaching the UI as localhost when it bound 127.0.0.1 still works;
// what matters is that a page on some other site is not one of these.
bool is_loopback_origin(const std::string& origin) {
    const auto scheme = origin.find("://");
    if (scheme == std::string::npos) return false;
    std::string host = origin.substr(scheme + 3);
    const auto slash = host.find('/');
    if (slash != std::string::npos) host = host.substr(0, slash);

    if (!host.empty() && host.front() == '[') {   // [::1]:5005
        const auto close = host.find(']');
        if (close == std::string::npos) return false;
        host = host.substr(1, close - 1);
    } else {
        const auto colon = host.rfind(':');
        if (colon != std::string::npos) host = host.substr(0, colon);
    }
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
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

// The context preamble, from the three fields the page collects. Shared by the
// live summarize route and the library re-run, so a summary made over an
// archived recording is given exactly what the same button would give it.
std::string context_preamble(const json& body, const std::string& summary_language) {
    const bool tr = (summary_language == "tr");
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
    return context;
}

// The accelerators the settings panel offers, named the way ggml names them.
// Sent on every settings read rather than cached: a laptop can gain or lose a
// card between one launch and the next.
json device_list_json() {
    json out = json::array();
    for (const ComputeDevice& d : list_devices()) {
        out.push_back({
            {"id", d.id},
            {"name", d.name},
            {"backend", d.backend},
            {"integrated", d.integrated},
            {"vram", d.vram_total},
        });
    }
    return out;
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

    token_ = random_token();
    const std::string token = token_;

    // -- request authorization ---------------------------------------------
    // Everything under /api/ that changes something was open to anything that
    // could reach loopback -- which, on this machine, is every process and
    // every page in the user's browser. A tab on an unrelated site could POST a
    // JSON body as text/plain and the browser would send it; the reply was
    // unreadable, but the change had already happened. Repointing output_dir
    // and then calling /api/library/delete turned that into removal of any
    // directory on disk.
    //
    // GET is left alone: it changes nothing, and the same-origin policy already
    // stops a foreign page reading what comes back. That also keeps the plain
    // <audio src="/api/library/audio?id=…"> element working.
    // When we are bound to loopback, every request has to be addressed to
    // loopback as well. Without this, GET was answered whatever hostname it
    // claimed -- and a name the attacker controls is the whole point of DNS
    // rebinding: once the browser believes an attacker's domain resolves here,
    // it treats this app as same-origin and can read transcripts, recordings,
    // settings and the page's own token straight out of GET responses. The
    // Origin and token checks below never see those requests, because they
    // genuinely are same-origin by then.
    //
    // A host that is not loopback means the user deliberately asked to serve
    // beyond this machine, so their Host header is their business.
    const bool loopback_only = is_loopback_origin("http://" + host_);
    const std::string bound_host = host_;

    svr.set_pre_routing_handler([token, loopback_only, bound_host](
                                    const httplib::Request& req,
                                    httplib::Response& res) {
        using Result = httplib::Server::HandlerResponse;

        if (loopback_only) {
            const std::string host = req.get_header_value("Host");
            // An absent Host is HTTP/1.0; it cannot carry a rebound name.
            if (!host.empty() && !is_loopback_origin("http://" + host) &&
                host.substr(0, host.find(':')) != bound_host) {
                send_error(res, L("That request was not addressed to this app.",
                                  "Bu istek bu uygulamaya gönderilmedi."), 403);
                return Result::Handled;
            }
        }

        if (req.method != "POST") return Result::Unhandled;

        const std::string origin = req.get_header_value("Origin");
        if (!origin.empty() && !is_loopback_origin(origin)) {
            send_error(res, L("That request did not come from this app.",
                              "Bu istek bu uygulamadan gelmedi."), 403);
            return Result::Handled;
        }
        // The page reads this out of its own <meta> tag, which only same-origin
        // script can reach. Sending it as a custom header also makes the
        // request non-simple, so a cross-origin attempt has to clear a
        // preflight that nothing here answers.
        if (req.get_header_value(kTokenHeader) != token) {
            send_error(res, L("This page is out of date — reload it.",
                              "Bu sayfa güncel değil — yenileyin."), 403);
            return Result::Handled;
        }
        return Result::Unhandled;
    });

    // -- static ------------------------------------------------------------
    auto serve_asset = [token](const std::string& path, httplib::Response& res) {
        const Asset* asset = find_asset(path);
        if (!asset) {
            res.status = 404;
            res.set_content("not found", "text/plain");
            return;
        }
        if (path == "index.html") {
            // Stamp this run's token into the page on the way out. Never
            // cached: a token from a previous run is worse than none, since it
            // would fail every button silently until the user reloaded.
            std::string html(asset->data, asset->size);
            const auto at = html.find(kTokenPlaceholder);
            if (at != std::string::npos) {
                html.replace(at, std::strlen(kTokenPlaceholder), token);
            }
            res.set_header("Cache-Control", "no-store");
            res.set_content(html, asset->content_type);
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
        } catch (const BusyError& e) {
            // Lost the race with another start: a 400, like every other "busy"
            // answer here, not a 500 that reads as a broken device.
            return send_error(res, e.what());
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

    // Two jobs under one button, because to the person pressing it they are
    // the same intent: stop what is happening. With a run in flight that means
    // asking the models to give up, and nothing is thrown away. Otherwise it is
    // the take or the result on screen that goes.
    //
    // {"job_only": true} asks for the first half alone. The library's Stop
    // button sends it: its run can finish while the question is on screen, and
    // falling back to cancel() then would discard the studio's take -- which
    // is not what stopping a re-run of some other recording agreed to.
    svr.Post("/api/cancel", [state](const httplib::Request& req,
                                    httplib::Response& res) {
        if (state->cancel_job()) {
            return send_json(res, json{{"ok", true}, {"stopped", "job"}});
        }
        const json body = parse_body(req);
        const auto job_only = body.find("job_only");
        if (job_only != body.end() && job_only->is_boolean() && job_only->get<bool>()) {
            return send_json(res, json{{"ok", true}, {"stopped", "none"}});
        }
        state->cancel();
        send_json(res, json{{"ok", true}, {"stopped", "take"}});
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

        const std::string name = paths::safe_filename(file.filename);
        // Named from the same CSPRNG as the session token. std::rand() is never
        // seeded anywhere in this program, so every run produced the identical
        // sequence: the first upload of every run landed on the same path in a
        // directory every user on the machine can write to. The write below
        // truncates whatever is already there and follows symlinks, so a name
        // that can be predicted is a name that can be waiting.
        const paths::fs::path tmp =
            paths::fs::temp_directory_path() /
            paths::from_utf8("transcriptor_upload_" + random_token() + "_" + name);

        if (!paths::write_file(tmp, file.content)) {
            return send_error(res,
                              L("The upload could not be written to the temp folder.",
                                "Yüklenen dosya geçici klasöre yazılamadı."), 500);
        }

        // False here now also covers a take too short to process, which used to
        // come back as {ok:true} with the error phase already set.
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

    // The update banner's button. Takes no parameters on purpose: the URL is
    // fixed above, so nothing from the page is ever handed to the OS opener.
    // Needed because target="_blank" goes nowhere inside the native webview.
    svr.Post("/api/open_releases", [](const httplib::Request&,
                                      httplib::Response& res) {
        send_json(res, json{{"ok", open_in_browser(kReleasesUrl)},
                            {"url", kReleasesUrl}});
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

        // Every saved transcript and summary, so the panel can offer the older
        // ones beside whatever the last re-run produced. The requested one wins
        // when it exists; otherwise this falls back to the first one on offer,
        // which is where a stale selection in the page should land.
        const auto variants_json = [](const std::vector<library::Variant>& vs) {
            json arr = json::array();
            for (const library::Variant& v : vs) {
                arr.push_back({{"name", v.name},
                               {"mtime", v.mtime},
                               {"structured", v.structured}});
            }
            return arr;
        };
        const auto pick = [](const std::vector<library::Variant>& vs,
                             const std::string& want) {
            for (const library::Variant& v : vs) {
                if (v.name == want) return want;
            }
            // The variants are listed original-first, so this is the original
            // wherever there is one. Returning "" regardless named a file that
            // need not exist: a session kept only as named versions opened with
            // an empty transcript panel and no way to reach what it held.
            return vs.empty() ? std::string() : vs.front().name;
        };

        const auto tx_variants  = library::transcript_variants(dir);
        const auto sum_variants = library::summary_variants(dir);
        body["transcripts"] = variants_json(tx_variants);
        body["summaries"]   = variants_json(sum_variants);

        const std::string tx_name =
            pick(tx_variants, req.get_param_value("transcript"));
        const std::string sum_name =
            pick(sum_variants, req.get_param_value("summary"));
        body["transcript_name"] = tx_name;
        body["summary_name"]    = sum_name;

        std::string raw;
        // transcript.json carries speakers and timestamps, so the library shows
        // it exactly the way the live transcript panel does; the .txt is only
        // the fallback for a session saved before the JSON existed.
        body["transcript"] = json(nullptr);
        if (paths::read_file(library::transcript_json_file(dir, tx_name), &raw)) {
            json parsed = json::parse(raw, nullptr, /*allow_exceptions=*/false);
            if (!parsed.is_discarded()) body["transcript"] = parsed;
        }
        body["transcript_text"] =
            paths::read_file(library::transcript_txt_file(dir, tx_name), &raw)
                ? json(raw) : json(nullptr);
        body["summary"] = json(nullptr);
        if (paths::read_file(library::summary_file(dir, sum_name), &raw)) {
            // Summaries written before the backends stripped reasoning still
            // carry the model's <think> block, so filter it on the way out.
            //
            // This used to rewrite summary.txt in place. A GET has none of the
            // protection the pre-routing handler gives a POST -- no token, no
            // Origin check -- so that made a file on disk writable by anything
            // that could reach this port, and it truncated the file before
            // checking whether the replacement could be written at all.
            // Filtering costs a pass over a few kilobytes; keep GET read-only.
            body["summary"] = json(llm::strip_reasoning(raw));
        }

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

    // Deletes one session folder and everything in it. resolve() only ever
    // yields a direct child of the output folder, and remove_session_dir()
    // checks the same boundary again before touching the disk.
    svr.Post("/api/library/delete", [state](const httplib::Request& req,
                                            httplib::Response& res) {
        const Settings s = state->settings_copy();
        const paths::fs::path dir =
            library::resolve(s.output_dir, get_string(parse_body(req), "id"));
        if (dir.empty()) {
            return send_error(res, L("That recording is no longer there.",
                                     "Bu kayıt artık yerinde değil."), 404);
        }
        // A folder still being written to is not deletable, and the state is
        // what knows that: the check and the removal happen together in there,
        // under the same lock that admits a re-run, because either one alone
        // leaves a window for the other.
        switch (state->delete_library_session(dir, s.output_dir)) {
            case AppState::DeleteOutcome::kBusy:
                return send_error(res, L("That recording is still being written.",
                                         "Bu kayıt hâlâ yazılıyor."));
            case AppState::DeleteOutcome::kFailed:
                return send_error(res, L("The folder could not be deleted.",
                                         "Klasör silinemedi."), 500);
            case AppState::DeleteOutcome::kOk:
                break;
        }
        send_json(res, json{{"ok", true}, {"id", paths::to_utf8(dir.filename())}});
    });

    // -- library re-runs ---------------------------------------------------
    // Run the models again over a recording already in the output folder. The
    // result replaces the session's original when `name` is empty, and is kept
    // beside it under that name otherwise. Neither disturbs the studio panels;
    // both take the one job slot, so the phase and progress the page already
    // polls tell the story.
    svr.Post("/api/library/transcribe", [state](const httplib::Request& req,
                                                httplib::Response& res) {
        if (state->recording()) {
            return send_error(res, L("A job is already running.", "İşlem sürüyor."));
        }
        const json body = parse_body(req);
        std::string error;
        if (!state->start_library_transcribe(get_string(body, "id"),
                                             trim(get_string(body, "name")), &error)) {
            return send_error(res, error);
        }
        send_json(res, json{{"ok", true}});
    });

    svr.Post("/api/library/summarize", [state](const httplib::Request& req,
                                               httplib::Response& res) {
        if (state->recording()) {
            return send_error(res, L("A job is already running.", "İşlem sürüyor."));
        }
        const json body = parse_body(req);
        std::string error;
        const std::string context =
            context_preamble(body, state->settings_copy().summary_language);
        if (!state->start_library_summarize(get_string(body, "id"),
                                            get_string(body, "source"),
                                            trim(get_string(body, "name")), context,
                                            trim(get_string(body, "template")),
                                            &error)) {
            return send_error(res, error);
        }
        send_json(res, json{{"ok", true}});
    });

    // -- transcribe --------------------------------------------------------
    // The manual counterpart of settings.auto_transcribe: runs the pipeline
    // over the audio that stopped short of it.
    svr.Post("/api/transcribe", [state](const httplib::Request&,
                                        httplib::Response& res) {
        if (state->recording()) {
            return send_error(res, L("A job is already running.", "İşlem sürüyor."));
        }
        // No separate processing() check: start_transcribe() takes the job slot
        // atomically and says no when it cannot. Checking here and starting
        // there left a window two requests could both walk through.
        std::string error;
        if (!state->start_transcribe(&error)) return send_error(res, error);
        send_json(res, json{{"ok", true}});
    });

    // -- summarize ---------------------------------------------------------
    svr.Post("/api/summarize", [state](const httplib::Request& req,
                                       httplib::Response& res) {
        // Recording counts as busy here too: stopping a take mid-summary would
        // find the job slot taken and have to park the audio instead of
        // transcribing it, which is a worse answer than "wait a moment".
        if (state->recording()) {
            return send_error(res, L("A job is already running.", "İşlem sürüyor."));
        }

        const json body = parse_body(req);
        const std::string context =
            context_preamble(body, state->settings_copy().summary_language);

        std::string error;
        if (!state->start_summarize(context, trim(get_string(body, "template")),
                                    &error)) {
            return send_error(res, error);
        }
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

        // Downloadable speech models, with what is already on disk marked.
        // This is the whole model picker now: there is no default and nothing
        // is fetched behind the user's back, so the panel has to say plainly
        // what each one is and which of them are here.
        json whisper = json::array();
        for (const models::WhisperModelSpec& m : models::whisper_catalog()) {
            const paths::fs::path file = models::whisper_model_file(m);
            std::error_code ec;
            whisper.push_back({{"id", m.id},
                               {"label", m.label},
                               {"note", m.note()},
                               {"size", models::human_size(m.approx_bytes)},
                               {"path", paths::to_utf8(file)},
                               {"downloaded", paths::fs::exists(file, ec)}});
        }

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
            {"whisper_catalog", whisper},
            {"whisper_ready", models::whisper_ready(s)},
            {"language", s.language},
            {"device", s.device},
            {"compute_type", s.compute_type},
            {"devices", device_list_json()},

            {"enable_diarization", s.enable_diarization},
            {"diar_supported", diarize::Diarizer::supported()},
            {"diar_segmentation_model", s.diar_segmentation_model},
            {"diar_embedding_model", s.diar_embedding_model},
            {"num_speakers", s.num_speakers},
            // Read-only: derived from the transcription language, not settable.
            // Reported so anything driving the API can see which value is in
            // force. Rounded because widening the float would otherwise print
            // 0.800000011920929. The settings panel keeps its own copy of the
            // table (CLTHR_BY_LANG in web/app.js) so it can preview a language
            // the user has picked but not yet saved.
            {"cluster_threshold", std::round(s.cluster_threshold() * 100.0) / 100.0},

            {"llm_backend", s.llm_backend},
            {"llm_model_path", s.llm_model_path},
            {"llm_ctx", s.llm_ctx},
            {"llm_gpu_layers", s.llm_gpu_layers},
            {"llm_max_tokens", s.llm_max_tokens},
            {"llm_thinking", s.llm_thinking},
            {"gguf_models", gguf},
            {"llm_catalog", catalog},
            {"model_download", state->model_download_json()},
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
            {"auto_transcribe", s.auto_transcribe},
            {"auto_summarize", s.auto_summarize},
            {"manage_vram", s.manage_vram},
            {"mic_gain", s.mic_gain},
            {"system_gain", s.system_gain},

            {"check_updates", s.check_updates},
            {"version", TRANSCRIPTOR_VERSION},
            {"repo", kRepoSlug},
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
        // "" means nothing chosen, which is the state a fresh install is in.
        // Anything else has to be a real model: a typo used to become a
        // download URL, and the failure surfaced mid-transcription.
        if (!s.whisper_model.empty() &&
            !models::whisper_catalog_entry(s.whisper_model) &&
            models::whisper_spec(s.whisper_model).url.empty()) {
            s.whisper_model.clear();
        }
        str("whisper_model_path", &s.whisper_model_path);
        str("language", &s.language);
        const bool device_sent = body.contains("device");
        str("device", &s.device);
        // Anything but "auto"/"cpu" has to name a device that is actually
        // here; a card that has gone away falls back to automatic rather than
        // silently landing on whichever device happens to hold that slot now.
        //
        // Only when the request says which device, though. The template menu
        // and the theme save one field each, and checking the stored device
        // on their behalf quietly rewrote a card that was merely unplugged --
        // the one the settings panel goes out of its way to keep showing.
        if (device_sent && s.device != "auto" && s.device != "cpu") {
            bool known = false;
            for (const ComputeDevice& d : list_devices()) {
                if (d.id == s.device) { known = true; break; }
            }
            if (!known) s.device = "auto";
        }
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
        flag("auto_transcribe", &s.auto_transcribe);
        flag("auto_summarize", &s.auto_summarize);
        flag("manage_vram", &s.manage_vram);
        flag("check_updates", &s.check_updates);
        flag("llm_thinking", &s.llm_thinking);

        clamped_float("mic_gain", &s.mic_gain, 0.0f, 4.0f);
        clamped_float("system_gain", &s.system_gain, 0.0f, 4.0f);
        // No "cluster_threshold" here: it follows the transcription language
        // now, so there is nothing for a POST to set. A body still carrying one
        // is ignored rather than refused -- an old page left open should not
        // fail to save the rest of its settings.
        clamped_int("num_speakers", &s.num_speakers, 0, 20);
        // 0 is "size it from the model and the machine"; anything the user
        // types by hand starts where a chat template alone stops fitting.
        clamped_int("llm_ctx", &s.llm_ctx, 0, 131072);
        if (s.llm_ctx > 0 && s.llm_ctx < 1024) s.llm_ctx = 1024;
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

        // Persist first, and say so if it did not work. The boolean used to be
        // discarded: the settings were adopted in memory, the page was told
        // {ok:true}, and everything the user had just edited was gone at the
        // next launch. Nothing is adopted on failure either, so what is on
        // screen still matches the app and Save can simply be pressed again.
        if (!s.save()) {
            return send_error(res,
                              L("The settings could not be saved to ",
                                "Ayarlar şuraya kaydedilemedi: ") +
                                  paths::to_utf8(Settings::config_path()),
                              500);
        }
        state->replace_settings(s);

        const json snapshot = state->state_json();
        send_json(res, json{{"ok", true}, {"device", snapshot["device"]}});
    });

    // -- LLM model discovery ----------------------------------------------
    svr.Post("/api/llm/models", [state](const httplib::Request& req,
                                        httplib::Response& res) {
        const json body = parse_body(req);
        try {
            // Both come from the open settings form, which may not have been
            // saved yet -- the backend as much as the URL.
            auto models = state->list_llm_models(get_string(body, "llm_backend"),
                                                 get_string(body, "llm_base_url"));
            send_json(res, json{{"models", models}});
        } catch (const std::exception& e) {
            // 200 with an error field, matching the Flask behaviour the UI expects.
            send_json(res, json{{"error", e.what()}, {"models", json::array()}});
        }
    });

    // -- model downloads ---------------------------------------------------
    // One route for both catalogs: there is a single download slot, and the
    // panel polls a single status whichever kind it started.
    svr.Post("/api/model/download", [state](const httplib::Request& req,
                                            httplib::Response& res) {
        const json body = parse_body(req);
        const std::string kind = trim(get_string(body, "kind"));
        const std::string id   = trim(get_string(body, "id"));
        if (id.empty()) return send_error(res, L("No model selected.", "Model seçilmedi."));

        std::string error;
        if (!state->start_model_download(kind.empty() ? "llm" : kind, id, &error)) {
            return send_error(res, error, 409);
        }
        send_json(res, json{{"ok", true}, {"download", state->model_download_json()}});
    });

    svr.Get("/api/model/download", [state](const httplib::Request&,
                                           httplib::Response& res) {
        send_json(res, state->model_download_json());
    });

    // Stops a download in flight. The thread tears itself down, so the UI just
    // keeps polling GET above and sees it go inactive like any other ending.
    svr.Post("/api/model/download/cancel", [state](const httplib::Request&,
                                                   httplib::Response& res) {
        if (!state->cancel_model_download()) {
            return send_error(res, L("No download is running.",
                                     "Çalışan bir indirme yok."), 409);
        }
        send_json(res, json{{"ok", true}});
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
