#include "llm/openai_backend.h"
#include "util/lang.h"
#include "util/utf8.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include <nlohmann/json.hpp>

#include <httplib.h>

namespace transcriptor::llm {

namespace {

struct Endpoint {
    std::string origin;   // "http://127.0.0.1:1234"
    std::string prefix;   // "/v1"
};

// Split "http://127.0.0.1:1234/v1" into an origin httplib can connect to and
// the path prefix routes hang off. Trailing slashes are ignored.
Endpoint split_base_url(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();

    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        // Bare "host:port" — assume http.
        url = "http://" + url;
    }
    const auto after_scheme = url.find("://") + 3;
    const auto path_start = url.find('/', after_scheme);

    Endpoint ep;
    if (path_start == std::string::npos) {
        ep.origin = url;
        ep.prefix = "";
    } else {
        ep.origin = url.substr(0, path_start);
        ep.prefix = url.substr(path_start);
    }
    return ep;
}

// The transcript travels inside this body, and whisper output is not always
// valid UTF-8; the strict default would throw over one stray byte and fail the
// summary with a json exception instead of sending it.
std::string request_body(const nlohmann::json& payload) {
    return payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

class OpenAIBackend : public Backend {
public:
    explicit OpenAIBackend(const Settings& s)
        : base_url_(s.llm_base_url), model_(s.llm_model),
          api_key_(s.llm_api_key.empty() ? "not-needed" : s.llm_api_key),
          timeout_(s.llm_timeout), temperature_(s.llm_temperature),
          max_tokens_(std::max(64, s.llm_max_tokens)),
          thinking_(s.llm_thinking) {}

    // Closing the window has to be able to end a request that is in flight.
    // Without this the backend inherited the base class's do-nothing abort:
    // shutdown() asked, then joined a worker parked inside the POST below, and
    // the process outlived its own window by up to llm_timeout -- ten minutes
    // by default, an hour at the top of the range.
    void request_abort() override {
        abort_.store(true);
        std::lock_guard<std::mutex> lock(client_mutex_);
        if (active_) active_->stop();   // closes the socket; the call returns
    }

    void reset_abort() override { abort_.store(false); }

    std::vector<std::string> list_models() override {
        auto client = make_client(10.0);
        if (!client) throw SummarizerError(tls_error());
        ActiveClient live(this, client.get());
        throw_if_aborted();   // same window as summarize(); same answer

        auto res = client->Get(endpoint_.prefix + "/models", headers());
        if (!res) {
            throw SummarizerError(
                L("The LLM server could not be reached (",
                  "LLM sunucusuna ulaşılamadı (") + base_url_ +
                L("). Is it running? [", "). Sunucu açık mı? [") +
                httplib::to_string(res.error()) + "]");
        }
        if (res->status != 200) {
            throw SummarizerError(L("The model list could not be fetched (HTTP ",
                                    "Model listesi alınamadı (HTTP ") +
                                  std::to_string(res->status) + ").");
        }

        auto j = nlohmann::json::parse(res->body, nullptr, false);
        std::vector<std::string> out;
        if (j.is_object() && j.contains("data") && j["data"].is_array()) {
            for (const auto& m : j["data"]) {
                if (m.is_object() && m.contains("id") && m["id"].is_string()) {
                    std::string id = m["id"].get<std::string>();
                    if (!id.empty()) out.push_back(std::move(id));
                }
            }
        }
        return out;
    }

    Availability available() override {
        std::vector<std::string> models;
        try {
            models = list_models();
        } catch (const SummarizerError& e) {
            return {false, e.what()};
        }
        if (models.empty()) {
            return {false, L("The server is up but has no model loaded. Load one "
                             "in LM Studio (e.g. Qwen).",
                             "Sunucu çalışıyor ama yüklü model yok. "
                             "LM Studio'da bir model yükleyin (ör. Qwen).")};
        }
        if (!model_.empty() &&
            std::find(models.begin(), models.end(), model_) == models.end()) {
            std::string list;
            for (std::size_t i = 0; i < models.size(); ++i) {
                if (i) list += ", ";
                list += models[i];
            }
            return {false, "Model '" + model_ +
                               L("' is not loaded. Available: ",
                                 "' yüklü değil. Mevcut: ") + list};
        }
        return {true, L("Ready. Model: ", "Hazır. Model: ") +
                          (model_.empty() ? models[0] : model_)};
    }

    Summary summarize(const SummaryRequest& req,
                      const ProgressFn& progress) override {
        // Not abort_.store(false): the flag is cleared when the job is admitted
        // (see reset_abort). Clearing it here threw away a shutdown raised
        // between the health check above and this call, and the request below
        // then ran to the full remote timeout -- ten minutes by default.
        throw_if_aborted();
        if (build_user_message(req).empty()) {
            throw SummarizerError(L("There is no text to summarize.",
                                    "Özetlenecek metin yok."));
        }

        std::string model = model_;
        if (model.empty()) {
            auto models = list_models();
            if (models.empty()) {
                throw SummarizerError(L("The server has no model loaded.",
                                        "Sunucuda yüklü model yok."));
            }
            model = models[0];
        }

        // Reasoning is budgeted beside the answer, not out of it. There is no
        // token-level control over a served model -- no way to close an
        // overrunning <think> the way the embedded backend does -- so the only
        // lever here is asking for the room, and asking the server not to think
        // at all when the setting is off.
        const int think_budget = thinking_ ? std::clamp(max_tokens_ / 2, 512, 2048) : 0;

        nlohmann::json payload = {
            {"model", model},
            {"temperature", temperature_},
            {"max_tokens", max_tokens_ + think_budget},
            {"stream", false},
            {"messages", nlohmann::json::array({
                {{"role", "system"}, {"content", resolve_system_prompt(req)}},
                {{"role", "user"},   {"content", build_user_message(req)}},
            })},
        };
        // llama-server, vLLM and LM Studio all pass this through to the chat
        // template; a server that has never heard of it answers 400, and the
        // request is worth one retry without the hint rather than failing a
        // summary over it.
        if (!thinking_) {
            payload["chat_template_kwargs"] = {{"enable_thinking", false}};
        }

        if (progress) {
            progress(L("Summarizing (", "Özetleniyor (") + model + ")…", -1.0);
        }

        auto client = make_client(timeout_);
        if (!client) throw SummarizerError(tls_error());
        ActiveClient live(this, client.get());

        // Refuse the request rather than issue it. ActiveClient calls stop() on
        // an abort it finds during setup, but a client that has not opened a
        // socket yet has nothing to shut down -- so the POST went out anyway,
        // the server answered, and a cancelled summary came back as a result.
        //
        // KNOWN LIMITATION: an abort landing between this check and the moment
        // httplib opens its socket still cannot stop the request. stop() is not
        // a sticky flag in cpp-httplib -- it acts on a connection in flight, and
        // there is none yet -- so closing the window properly would mean owning
        // the socket ourselves. What is left is microseconds wide rather than
        // "any time before the POST", and the check after the response keeps a
        // cancelled answer from being used; the cost is that a shutdown in that
        // instant still waits out the remote timeout.
        throw_if_aborted();

        auto res = client->Post(endpoint_.prefix + "/chat/completions", headers(),
                                request_body(payload), "application/json");
        if (res && res->status == 400 && payload.contains("chat_template_kwargs")) {
            // The server does not know the kwarg. Ask again without it and let
            // strip_reasoning() clean up after whatever it sends back.
            throw_if_aborted();
            payload.erase("chat_template_kwargs");
            res = client->Post(endpoint_.prefix + "/chat/completions", headers(),
                               request_body(payload), "application/json");
        }
        // The abort is authoritative even when the answer beat it: nothing
        // downstream wants a summary for an operation the user stopped.
        throw_if_aborted();
        if (!res) {
            // A socket closed by request_abort() lands here too; say which it
            // was, rather than reporting the shutdown as a server failure.
            if (abort_.load()) {
                throw SummarizerError(L("Summarizing was cancelled.",
                                        "Özetleme iptal edildi."));
            }
            throw SummarizerError(L("The summary request failed: ",
                                    "Özetleme isteği başarısız: ") +
                                  httplib::to_string(res.error()));
        }
        if (res->status != 200) {
            throw SummarizerError(L("The summary request failed (HTTP ",
                                    "Özetleme isteği başarısız (HTTP ") +
                                  std::to_string(res->status) + "): " +
                                  utf8::head(res->body, 400));
        }

        auto j = nlohmann::json::parse(res->body, nullptr, false);
        if (j.is_discarded() || !j.contains("choices") || !j["choices"].is_array() ||
            j["choices"].empty()) {
            throw SummarizerError(L("Unexpected response format: ",
                                    "Beklenmeyen yanıt biçimi: ") +
                                  utf8::head(res->body, 400));
        }
        const auto& msg = j["choices"][0]["message"];
        if (!msg.is_object()) {
            throw SummarizerError(L("Unexpected response format: ",
                                    "Beklenmeyen yanıt biçimi: ") +
                                  utf8::head(res->body, 400));
        }

        // A server that separates the two sends the answer in content and the
        // chain of thought in reasoning_content -- and sends content as null,
        // not as a string, when the model never got past thinking. Reading only
        // content and demanding a string reported that as a malformed response.
        std::string served;
        if (msg.contains("content") && msg["content"].is_string()) {
            served = msg["content"].get<std::string>();
        } else if (!msg.contains("reasoning_content")) {
            throw SummarizerError(L("Unexpected response format: ",
                                    "Beklenmeyen yanıt biçimi: ") +
                                  utf8::head(res->body, 400));
        }

        // Reasoning comes off here, where the model's text arrives: a served
        // model that inlines <think> in the content is no different from the
        // embedded one, and no caller should have to remember.
        std::string content = strip_reasoning(served);

        // All reasoning and no answer. The budget ran out inside the <think>
        // block, and nothing here can close it the way the embedded backend
        // does -- so this is the one place that advice is still the answer.
        if (content.empty() && reasoned(msg, served)) {
            throw SummarizerError(
                L("The model spent the whole request on reasoning and never "
                  "answered. Raise the maximum answer length in Settings, or "
                  "turn off \"Let the model think first\".",
                  "Model, isteğin tamamını düşünmeye harcadı ve yanıt vermedi. "
                  "Ayarlar'dan azami yanıt uzunluğunu artırın veya \"Önce "
                  "düşünmesine izin ver\" seçeneğini kapatın."));
        }
        if (content.empty()) {
            throw SummarizerError(L("The LLM returned an empty answer.",
                                    "LLM boş yanıt döndürdü."));
        }
        // "length" is how every OpenAI-compatible server says it stopped on
        // max_tokens rather than because the model was finished.
        const auto& choice = j["choices"][0];
        const bool cut = choice.contains("finish_reason") &&
                         choice["finish_reason"].is_string() &&
                         choice["finish_reason"].get<std::string>() == "length";
        return Summary{content, cut};
    }

private:
    // Did the model think, whether it inlined the block in the content or the
    // server lifted it into a field of its own?
    static bool reasoned(const nlohmann::json& msg, const std::string& served) {
        if (served.find_first_not_of(" \t\r\n") != std::string::npos) return true;
        auto it = msg.find("reasoning_content");
        return it != msg.end() && it->is_string() &&
               it->get<std::string>().find_first_not_of(" \t\r\n") !=
                   std::string::npos;
    }

    // Publishes the client for the length of one call, so request_abort() has
    // something to close and never a dangling pointer after it returns.
    class ActiveClient {
    public:
        ActiveClient(OpenAIBackend* owner, httplib::Client* client)
            : owner_(owner) {
            std::lock_guard<std::mutex> lock(owner_->client_mutex_);
            owner_->active_ = client;
            // An abort that landed while this call was being set up would
            // otherwise be missed entirely; stop() now, and the request fails
            // immediately instead of running to its timeout.
            if (owner_->abort_.load()) client->stop();
        }
        ~ActiveClient() {
            std::lock_guard<std::mutex> lock(owner_->client_mutex_);
            owner_->active_ = nullptr;
        }
        ActiveClient(const ActiveClient&) = delete;
        ActiveClient& operator=(const ActiveClient&) = delete;

    private:
        OpenAIBackend* owner_;
    };

    void throw_if_aborted() const {
        if (abort_.load()) {
            throw SummarizerError(L("Summarizing was cancelled.",
                                    "Özetleme iptal edildi."));
        }
    }

    httplib::Headers headers() const {
        return {{"Authorization", "Bearer " + api_key_},
                {"Content-Type", "application/json"}};
    }

    std::string tls_error() const {
        return L("HTTPS addresses are not supported (",
                 "HTTPS adresleri desteklenmiyor (") + base_url_ +
               L("). Use http:// for local servers.",
                 "). Yerel sunucular için http:// kullanın.");
    }

    std::unique_ptr<httplib::Client> make_client(double timeout_sec) {
        endpoint_ = split_base_url(base_url_);
        // Built without a TLS backend on purpose — the target is localhost.
        if (endpoint_.origin.rfind("https://", 0) == 0) return nullptr;

        auto client = std::make_unique<httplib::Client>(endpoint_.origin);
        client->set_connection_timeout(std::min(10.0, timeout_sec));
        client->set_read_timeout(static_cast<time_t>(timeout_sec), 0);
        client->set_write_timeout(static_cast<time_t>(timeout_sec), 0);
        client->set_follow_location(true);
        return client;
    }

    std::string base_url_;
    std::string model_;
    std::string api_key_;
    double      timeout_;
    float       temperature_;
    int         max_tokens_;
    bool        thinking_;
    Endpoint    endpoint_;

    // The request in flight, and the flag that says a stop was asked for. Both
    // are touched from the shutdown thread while the worker sits in the call.
    std::mutex        client_mutex_;
    httplib::Client*  active_ = nullptr;
    std::atomic<bool> abort_{false};
};

}  // namespace

std::unique_ptr<Backend> make_openai_backend(const Settings& settings) {
    return std::make_unique<OpenAIBackend>(settings);
}

}  // namespace transcriptor::llm
