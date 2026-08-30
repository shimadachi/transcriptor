#include "llm/openai_backend.h"
#include "util/lang.h"

#include <algorithm>

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

class OpenAIBackend : public Backend {
public:
    explicit OpenAIBackend(const Settings& s)
        : base_url_(s.llm_base_url), model_(s.llm_model),
          api_key_(s.llm_api_key.empty() ? "not-needed" : s.llm_api_key),
          timeout_(s.llm_timeout), temperature_(s.llm_temperature),
          max_tokens_(s.llm_max_tokens) {}

    std::vector<std::string> list_models() override {
        auto client = make_client(10.0);
        if (!client) throw SummarizerError(tls_error());

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

    std::string summarize(const SummaryRequest& req,
                          const ProgressFn& progress) override {
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

        nlohmann::json payload = {
            {"model", model},
            {"temperature", temperature_},
            {"max_tokens", max_tokens_},
            {"stream", false},
            {"messages", nlohmann::json::array({
                {{"role", "system"}, {"content", resolve_system_prompt(req)}},
                {{"role", "user"},   {"content", build_user_message(req)}},
            })},
        };

        if (progress) {
            progress(L("Summarizing (", "Özetleniyor (") + model + ")…", -1.0);
        }

        auto client = make_client(timeout_);
        if (!client) throw SummarizerError(tls_error());

        auto res = client->Post(endpoint_.prefix + "/chat/completions", headers(),
                                payload.dump(), "application/json");
        if (!res) {
            throw SummarizerError(L("The summary request failed: ",
                                    "Özetleme isteği başarısız: ") +
                                  httplib::to_string(res.error()));
        }
        if (res->status != 200) {
            throw SummarizerError(L("The summary request failed (HTTP ",
                                    "Özetleme isteği başarısız (HTTP ") +
                                  std::to_string(res->status) + "): " +
                                  res->body.substr(0, 400));
        }

        auto j = nlohmann::json::parse(res->body, nullptr, false);
        if (j.is_discarded() || !j.contains("choices") || !j["choices"].is_array() ||
            j["choices"].empty()) {
            throw SummarizerError(L("Unexpected response format: ",
                                    "Beklenmeyen yanıt biçimi: ") +
                                  res->body.substr(0, 400));
        }
        const auto& msg = j["choices"][0]["message"];
        if (!msg.is_object() || !msg.contains("content") ||
            !msg["content"].is_string()) {
            throw SummarizerError(L("Unexpected response format: ",
                                    "Beklenmeyen yanıt biçimi: ") +
                                  res->body.substr(0, 400));
        }

        std::string content = msg["content"].get<std::string>();
        const auto b = content.find_first_not_of(" \t\r\n");
        const auto e = content.find_last_not_of(" \t\r\n");
        content = (b == std::string::npos) ? "" : content.substr(b, e - b + 1);

        if (content.empty()) {
            throw SummarizerError(L("The LLM returned an empty answer.",
                                    "LLM boş yanıt döndürdü."));
        }
        return content;
    }

private:
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
    Endpoint    endpoint_;
};

}  // namespace

std::unique_ptr<Backend> make_openai_backend(const Settings& settings) {
    return std::make_unique<OpenAIBackend>(settings);
}

}  // namespace transcriptor::llm
