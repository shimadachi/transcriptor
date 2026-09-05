#include "llm/llama_backend.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <ggml-backend.h>
#include <llama.h>

#include "llm/templates.h"
#include "util/cpu.h"
#include "util/lang.h"
#include "util/paths.h"

namespace transcriptor::llm {

namespace {

// Room reserved inside the context window for the system prompt, the chat
// template's own tokens, and the answer we are about to generate.
constexpr int kPromptMargin = 512;

std::once_flag g_backend_once;

void quiet_log(ggml_log_level level, const char* text, void* /*user_data*/) {
    // llama.cpp is chatty on stderr; only surface real problems.
    if (level >= GGML_LOG_LEVEL_ERROR && text) std::fputs(text, stderr);
}

void init_backend_once() {
    std::call_once(g_backend_once, [] {
        if (std::getenv("TRANSCRIPTOR_LLAMA_VERBOSE") == nullptr) llama_log_set(quiet_log, nullptr);
        llama_backend_init();
    });
}

int default_threads(int configured) {
    if (configured > 0) return configured;
    // Was hardware_concurrency/2, which is the right answer on a two-way SMT
    // machine and an undercount everywhere else -- it is what util/cpu.cpp
    // falls back to when it cannot read the topology. Ask properly instead.
    return static_cast<int>(std::max(1u, util::physical_cores()));
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Split a transcript into chunks of at most `max_chars`, preferring line
// boundaries so a speaker turn is never cut in half.
std::vector<std::string> split_transcript(const std::string& text,
                                          std::size_t max_chars) {
    std::vector<std::string> chunks;
    if (text.size() <= max_chars) {
        chunks.push_back(text);
        return chunks;
    }

    std::string current;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::string line = (nl == std::string::npos)
                               ? text.substr(pos)
                               : text.substr(pos, nl - pos + 1);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;

        // A single line longer than the budget has to be cut mid-sentence.
        while (line.size() > max_chars) {
            if (!current.empty()) {
                chunks.push_back(current);
                current.clear();
            }
            chunks.push_back(line.substr(0, max_chars));
            line = line.substr(max_chars);
        }
        if (current.size() + line.size() > max_chars && !current.empty()) {
            chunks.push_back(current);
            current.clear();
        }
        current += line;
    }
    if (!trim(current).empty()) chunks.push_back(current);
    return chunks;
}

class LlamaBackend : public Backend {
public:
    LlamaBackend(const Settings& s, const DeviceInfo& device)
        : model_path_(s.llm_model_path), n_ctx_(std::max(1024, s.llm_ctx)),
          n_gpu_layers_(device.use_gpu() ? s.llm_gpu_layers : 0),
          gpu_registry_index_(device.use_gpu() ? device.registry_index : -1),
          threads_(default_threads(s.llm_threads)),
          max_tokens_(std::max(64, s.llm_max_tokens)),
          temperature_(s.llm_temperature) {}

    ~LlamaBackend() override { unload(); }

    std::vector<std::string> list_models() override { return discover_gguf_models(); }

    Availability available() override {
        const paths::fs::path path = resolve_model_path();
        if (path.empty()) {
            return {false,
                    L("No summarizer model is selected. Download one from "
                      "Settings → Summarizer → \"Download a ready-made model\", "
                      "or drop a .gguf into ",
                      "Özet modeli seçilmemiş. Ayarlar → Özetleyici → \"Hazır "
                      "model indir\" ile bir model indirin ya da ") +
                    paths::to_utf8(paths::models_dir()) +
                    L(".", " klasörüne bir .gguf koyun.")};
        }
        std::error_code ec;
        if (!paths::fs::exists(path, ec)) {
            return {false, L("Summarizer model not found: ",
                             "Özet modeli bulunamadı: ") + paths::to_utf8(path)};
        }
        return {true, L("Ready. Model: ", "Hazır. Model: ") +
                          paths::to_utf8(path.filename())};
    }

    void unload() override {
        std::lock_guard<std::mutex> lock(mutex_);
        free_context();
        if (model_) {
            llama_model_free(model_);
            model_ = nullptr;
            loaded_path_.clear();
        }
    }

    void request_abort() override { abort_.store(true); }

    std::string summarize(const SummaryRequest& req,
                          const ProgressFn& progress) override {
        abort_.store(false);

        const std::string transcript = trim(req.transcript);
        if (transcript.empty()) {
            throw SummarizerError(L("There is no text to summarize.",
                                    "Özetlenecek metin yok."));
        }

        std::lock_guard<std::mutex> lock(mutex_);
        load(progress);

        const std::string system = resolve_system_prompt(req);

        // Budget in characters, converted from the token budget with a
        // deliberately pessimistic ~2.5 chars/token (Turkish tokenizes densely).
        const int prompt_budget_tokens = n_ctx_ - max_tokens_ - kPromptMargin;
        if (prompt_budget_tokens < 256) {
            throw SummarizerError(
                L("The context window is too small. Raise the context size in "
                  "Settings, or lower the answer length.",
                  "Bağlam penceresi çok küçük. Ayarlar'dan bağlam boyutunu "
                  "artırın veya yanıt uzunluğunu azaltın."));
        }
        const auto budget_chars =
            static_cast<std::size_t>(prompt_budget_tokens) * 5 / 2;

        SummaryRequest work = req;
        work.transcript = transcript;

        if (count_tokens(build_user_message(work)) <= prompt_budget_tokens) {
            return generate(system, build_user_message(work), progress);
        }

        // Too long for one pass: summarize chunk by chunk, then summarize the
        // notes. Keeps a two-hour meeting usable on an 8k-context model.
        const auto chunks = split_transcript(transcript, budget_chars);
        std::string notes;
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            if (abort_.load()) {
                throw SummarizerError(L("Summarizing was cancelled.",
                                        "Özetleme iptal edildi."));
            }
            if (progress) {
                progress(L("Long recording: summarizing part ",
                           "Uzun kayıt: bölüm ") + std::to_string(i + 1) + "/" +
                             std::to_string(chunks.size()) +
                             L("…", " özetleniyor…"),
                         static_cast<double>(i) / static_cast<double>(chunks.size()));
            }
            SummaryRequest part = work;
            part.transcript = chunks[i];
            notes += generate(partial_prompt(req.language),
                              build_user_message(part), nullptr);
            notes += "\n\n";
        }

        if (progress) {
            progress(L("Merging the section notes…",
                       "Bölüm notları birleştiriliyor…"), -1.0);
        }
        SummaryRequest final_req = work;
        final_req.transcript = notes;
        final_req.context.clear();   // already folded into the notes
        return generate(system, build_user_message(final_req), progress);
    }

private:
    static std::string partial_prompt(const std::string& language) {
        if (language == "tr") {
            return "Uzun bir kaydın bir bölümünü alıyorsun. Bu bölümdeki "
                   "konuşulanları maddeler halinde, sadık biçimde not al. "
                   "Yorum ekleme, sonuç çıkarma — sadece geçen konular, "
                   "kararlar ve aksiyonlar.";
        }
        return "You receive one section of a long recording. Take faithful "
               "bulleted notes of what was said in this section. Do not "
               "editorialize or conclude — only topics, decisions and actions.";
    }

    paths::fs::path resolve_model_path() const {
        if (!model_path_.empty()) return paths::expand_user(model_path_);
        // Fall back to the single GGUF in the models dir, if there is exactly one.
        const auto found = discover_gguf_models();
        if (found.size() == 1) return paths::from_utf8(found.front());
        return {};
    }

    void free_context() {
        if (ctx_) {
            llama_free(ctx_);
            ctx_ = nullptr;
        }
    }

    void load(const ProgressFn& progress) {
        const paths::fs::path path = resolve_model_path();
        if (path.empty()) throw SummarizerError(available().message);

        const std::string path_utf8 = paths::to_utf8(path);
        if (model_ && loaded_path_ == path_utf8) return;

        init_backend_once();
        unload_locked();

        if (progress) {
            progress(L("Loading the summarizer model (",
                       "Özet modeli yükleniyor (") +
                         paths::to_utf8(path.filename()) + ")…", -1.0);
        }

        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = n_gpu_layers_;

        // Offload to the card the user picked, not to whatever llama would
        // rank first. The list is NULL-terminated and read during the load, so
        // it only has to outlive this call.
        ggml_backend_dev_t chosen[2] = {nullptr, nullptr};
        if (gpu_registry_index_ >= 0 &&
            static_cast<size_t>(gpu_registry_index_) < ggml_backend_dev_count()) {
            chosen[0] = ggml_backend_dev_get(static_cast<size_t>(gpu_registry_index_));
            if (chosen[0]) mparams.devices = chosen;
        }

        model_ = llama_model_load_from_file(path_utf8.c_str(), mparams);
        if (!model_) {
            throw SummarizerError(L("The summarizer model could not be loaded: ",
                                    "Özet modeli yüklenemedi: ") + path_utf8);
        }
        loaded_path_ = path_utf8;
        vocab_ = llama_model_get_vocab(model_);
    }

    void unload_locked() {
        free_context();
        if (model_) {
            llama_model_free(model_);
            model_ = nullptr;
            loaded_path_.clear();
        }
    }

    // A fresh context per generation, so no KV state leaks between summaries.
    void reset_context() {
        free_context();
        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx       = static_cast<uint32_t>(n_ctx_);
        cparams.n_batch     = 512;
        cparams.n_threads   = threads_;
        cparams.n_threads_batch = threads_;
        ctx_ = llama_init_from_model(model_, cparams);
        if (!ctx_) {
            throw SummarizerError(L("The LLM context could not be created.",
                                    "LLM bağlamı oluşturulamadı."));
        }
    }

    std::vector<llama_token> tokenize(const std::string& text, bool add_special) {
        const int upper_bound = static_cast<int>(text.size()) + 16;
        std::vector<llama_token> out(static_cast<std::size_t>(upper_bound));
        const int n = llama_tokenize(vocab_, text.c_str(),
                                     static_cast<int32_t>(text.size()), out.data(),
                                     upper_bound, add_special, /*parse_special=*/true);
        if (n < 0) {
            out.resize(static_cast<std::size_t>(-n));
            const int n2 = llama_tokenize(vocab_, text.c_str(),
                                          static_cast<int32_t>(text.size()),
                                          out.data(), -n, add_special, true);
            out.resize(static_cast<std::size_t>(std::max(0, n2)));
        } else {
            out.resize(static_cast<std::size_t>(n));
        }
        return out;
    }

    int count_tokens(const std::string& text) {
        if (!model_) return 0;
        return static_cast<int>(tokenize(text, true).size());
    }

    // Render the system+user pair through the model's own chat template, so
    // each GGUF gets the prompt format it was trained on.
    std::string apply_chat_template(const std::string& system,
                                    const std::string& user) {
        const char* tmpl = llama_model_chat_template(model_, /*name=*/nullptr);

        std::vector<llama_chat_message> messages = {
            {"system", system.c_str()},
            {"user", user.c_str()},
        };

        std::vector<char> buf(system.size() + user.size() + 2048);
        int32_t n = llama_chat_apply_template(tmpl, messages.data(), messages.size(),
                                              /*add_ass=*/true, buf.data(),
                                              static_cast<int32_t>(buf.size()));
        if (n > static_cast<int32_t>(buf.size())) {
            buf.resize(static_cast<std::size_t>(n));
            n = llama_chat_apply_template(tmpl, messages.data(), messages.size(),
                                          true, buf.data(),
                                          static_cast<int32_t>(buf.size()));
        }
        if (n < 0) {
            // No usable template in the GGUF — fall back to a plain layout.
            return system + "\n\n" + user + "\n\n";
        }
        return std::string(buf.data(), static_cast<std::size_t>(n));
    }

    std::string token_to_text(llama_token id) {
        char buf[256];
        const int n = llama_token_to_piece(vocab_, id, buf, sizeof(buf), 0,
                                           /*special=*/false);
        if (n <= 0) return {};
        return std::string(buf, static_cast<std::size_t>(n));
    }

    std::string generate(const std::string& system, const std::string& user,
                         const ProgressFn& progress) {
        reset_context();

        const std::string prompt = apply_chat_template(system, user);
        std::vector<llama_token> tokens = tokenize(prompt, /*add_special=*/true);

        if (static_cast<int>(tokens.size()) >= n_ctx_) {
            throw SummarizerError(
                L("The text does not fit the context window (",
                  "Metin bağlam penceresine sığmadı (") +
                std::to_string(tokens.size()) + " / " + std::to_string(n_ctx_) +
                L(" tokens). Raise the context size in Settings.",
                  " token). Ayarlar'dan bağlam boyutunu artırın."));
        }

        if (progress) progress(L("Summarizing…", "Özetleniyor…"), 0.0);

        // Ingest the prompt in batches the context was sized for.
        constexpr int kBatch = 512;
        for (std::size_t off = 0; off < tokens.size(); off += kBatch) {
            const int n = static_cast<int>(
                std::min<std::size_t>(kBatch, tokens.size() - off));
            llama_batch batch = llama_batch_get_one(tokens.data() + off, n);
            if (llama_decode(ctx_, batch) != 0) {
                throw SummarizerError(L("The LLM prompt could not be processed "
                                        "(llama_decode).",
                                        "LLM istemi işlenemedi (llama_decode)."));
            }
            if (abort_.load()) {
                throw SummarizerError(L("Summarizing was cancelled.",
                                        "Özetleme iptal edildi."));
            }
        }

        llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
        sparams.no_perf = true;
        llama_sampler* sampler = llama_sampler_chain_init(sparams);
        // Narrow the candidate set first — the penalty sampler is slow over a
        // full vocabulary — then penalize repeats. Summarization at a low
        // temperature is prone to looping on a bullet it already emitted, and
        // small models loop badly without this.
        llama_sampler_chain_add(sampler, llama_sampler_init_top_k(40));
        llama_sampler_chain_add(sampler, llama_sampler_init_top_p(0.95f, 1));
        llama_sampler_chain_add(sampler, llama_sampler_init_penalties(
                                             llama_vocab_n_tokens(vocab_),
                                             /*penalty_last_n=*/256,
                                             /*penalty_repeat=*/1.1f,
                                             /*penalty_freq=*/0.0f,
                                             /*penalty_present=*/0.0f));
        llama_sampler_chain_add(sampler, llama_sampler_init_temp(temperature_));
        llama_sampler_chain_add(sampler, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

        struct SamplerGuard {
            llama_sampler* s;
            ~SamplerGuard() { llama_sampler_free(s); }
        } guard{sampler};

        std::string out;
        int n_past = static_cast<int>(tokens.size());

        for (int i = 0; i < max_tokens_; ++i) {
            if (abort_.load()) {
                throw SummarizerError(L("Summarizing was cancelled.",
                                        "Özetleme iptal edildi."));
            }

            llama_token id = llama_sampler_sample(sampler, ctx_, -1);
            if (llama_vocab_is_eog(vocab_, id)) break;

            out += token_to_text(id);

            if (progress && (i % 16 == 0)) {
                progress("", static_cast<double>(i) / max_tokens_);
            }

            if (++n_past >= n_ctx_) break;   // ran out of window

            llama_batch batch = llama_batch_get_one(&id, 1);
            if (llama_decode(ctx_, batch) != 0) {
                throw SummarizerError(L("LLM generation broke off (llama_decode).",
                                        "LLM üretimi kesildi (llama_decode)."));
            }
        }

        // The weights are the biggest thing on the GPU; drop the KV cache now.
        free_context();

        // Reasoning comes off here, at the one point raw model output becomes a
        // string: a thinking model's <think> block is not an answer, and the
        // chunked path feeds these straight back in as the notes to merge.
        const bool had_text = !trim(out).empty();
        out = strip_reasoning(out);
        if (out.empty()) {
            // All reasoning and no answer: the token budget ran out inside the
            // <think> block. Say that, rather than "empty answer".
            if (had_text) {
                throw SummarizerError(
                    L("The model used the whole answer budget on reasoning. "
                      "Raise the maximum answer length in Settings.",
                      "Model, yanıt bütçesinin tamamını düşünmeye harcadı. "
                      "Ayarlar'dan maksimum yanıt uzunluğunu artırın."));
            }
            throw SummarizerError(L("The LLM returned an empty answer.",
                                    "LLM boş yanıt döndürdü."));
        }
        if (progress) progress("", 1.0);
        return out;
    }

    std::string   model_path_;
    int           n_ctx_;
    int           n_gpu_layers_;
    int           gpu_registry_index_;   // -1 = CPU only
    int           threads_;
    int           max_tokens_;
    float         temperature_;

    std::mutex        mutex_;
    std::atomic<bool> abort_{false};

    llama_model*       model_ = nullptr;
    llama_context*     ctx_   = nullptr;
    const llama_vocab* vocab_ = nullptr;
    std::string        loaded_path_;
};

}  // namespace

std::vector<std::string> discover_gguf_models() {
    std::vector<std::string> out;
    std::error_code ec;

    const paths::fs::path dir = paths::models_dir();
    if (!paths::fs::exists(dir, ec)) return out;

    for (paths::fs::recursive_directory_iterator it(dir, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".gguf") out.push_back(paths::to_utf8(it->path()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::unique_ptr<Backend> make_llama_backend(const Settings& settings,
                                            const DeviceInfo& device) {
    return std::make_unique<LlamaBackend>(settings, device);
}

}  // namespace transcriptor::llm
