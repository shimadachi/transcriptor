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

#include "llm/chunking.h"
#include "llm/templates.h"
#include "util/cpu.h"
#include "util/lang.h"
#include "util/paths.h"

namespace transcriptor::llm {

namespace {

// Starting guess only, for turning a token budget into a byte budget to slice
// on. Every slice is then measured for real -- see fits() -- so this being
// wrong costs an extra split, not an overflow.
constexpr int kPromptMargin = 512;

// How many times the section notes may be reduced before the merge. Three
// passes take an 8k model from roughly 500 sections to one; beyond that the
// notes are not shrinking and another pass would only burn tokens.
constexpr int kMaxReducePasses = 3;

// Bounds on the window when the user leaves the context size on automatic.
//
// The ceiling is not a hardware limit: a model trained at 256k will happily
// report it, and sizing the KV cache to that costs gigabytes to summarize a
// ten-minute standup. 32k tokens is around an hour of speech, which is the
// point past which chunking is the better tool anyway.
constexpr int kAutoCtxCeiling = 32768;
constexpr int kAutoCtxFloor   = 4096;

// What the KV cache may claim when it lives in host RAM, where there is no
// portable way to ask what is free. On a GPU the driver is asked instead.
constexpr long long kHostKvBudget = 2LL << 30;   // 2 GiB

// Contexts are rounded up to this, so a hundred slightly different transcript
// lengths do not each allocate a slightly different cache.
constexpr int kCtxGrain = 256;

// Tokens per llama_decode call while the prompt is being ingested.
constexpr int kBatch = 512;

// Reasoning's allowance when the user turns thinking on, derived from the
// answer length rather than being a knob of its own -- the whole point of this
// setting is to stop asking people to balance two numbers by hand. Half an
// answer is enough for a model to weigh a meeting's threads against each other;
// the floor keeps it useful when the answer is short, and the ceiling stops a
// long-winded model from thinking for a minute before it starts writing.
int think_budget_for(int max_tokens) {
    return std::clamp(max_tokens / 2, 512, 2048);
}

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

// ASCII case-insensitive substring test, which is all a template scan needs.
bool contains_ci(const std::string& hay, const std::string& needle) {
    const auto lower = [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    };
    if (needle.empty() || needle.size() > hay.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        std::size_t j = 0;
        while (j < needle.size() && lower(hay[i + j]) == lower(needle[j])) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

class LlamaBackend : public Backend {
public:
    LlamaBackend(const Settings& s, const DeviceInfo& device)
        : model_path_(s.llm_model_path),
          ctx_setting_(s.llm_ctx > 0 ? std::max(1024, s.llm_ctx) : 0),
          n_gpu_layers_(device.use_gpu() ? s.llm_gpu_layers : 0),
          gpu_registry_index_(device.use_gpu() ? device.registry_index : -1),
          threads_(default_threads(s.llm_threads)),
          max_tokens_(std::max(64, s.llm_max_tokens)),
          want_thinking_(s.llm_thinking),
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
    void reset_abort() override { abort_.store(false); }

    Summary summarize(const SummaryRequest& req,
                      const ProgressFn& progress) override {
        // Not abort_.store(false): see reset_abort(). Loading a GGUF takes long
        // enough that a shutdown landing just before this line, and then being
        // erased by it, held the window open for the whole load.
        if (abort_.load()) {
            throw SummarizerError(L("Summarizing was cancelled.",
                                    "Özetleme iptal edildi."));
        }

        const std::string transcript = trim(req.transcript);
        if (transcript.empty()) {
            throw SummarizerError(L("There is no text to summarize.",
                                    "Özetlenecek metin yok."));
        }

        std::lock_guard<std::mutex> lock(mutex_);
        load(progress);

        const std::string system  = resolve_system_prompt(req);
        const std::string section_system = partial_prompt(req.language);

        // Sections are sliced against the note-taking pass, which never thinks,
        // so its reserve is the answer alone. Byte budget from a deliberately
        // pessimistic ~2.5 chars/token; only a starting guess, since fits()
        // measures each slice for real.
        const int prompt_budget_tokens =
            ctx_max_ - reserve_for(false) - kPromptMargin;
        if (prompt_budget_tokens < 256) {
            throw SummarizerError(
                L("An answer of ", "Yanıt uzunluğu (") +
                std::to_string(max_tokens_) +
                L(" tokens leaves no room for the transcript in this model's ",
                  " token), bu modelin ") +
                std::to_string(ctx_max_) +
                L("-token window. Lower the maximum answer length in Settings.",
                  " tokenlik penceresinde transkripte yer bırakmıyor. "
                  "Ayarlar'dan azami yanıt uzunluğunu azaltın."));
        }
        const auto budget_chars =
            static_cast<std::size_t>(prompt_budget_tokens) * 5 / 2;

        SummaryRequest work = req;
        work.transcript = transcript;

        // Measure the prompt that will actually be fed to the model -- system
        // prompt, user message and the chat template's own tokens -- with the
        // answer's tokens reserved. Counting only the user message and trusting
        // a flat 512-token margin to cover the rest meant an edited system
        // prompt could push the real prompt past the window, and left nothing
        // set aside for the reply.
        const bool think = thinking_allowed();
        Summary out;
        if (fits(system, build_user_message(work), think)) {
            out.text = generate(system, build_user_message(work), think, progress,
                                &out.cut_short);
            return out;
        }

        // Too long for one pass: summarize section by section, then summarize
        // the notes. Keeps a two-hour meeting usable on an 8k-context model.
        // Sections are measured against the note-taking prompt, which is what
        // summarize_sections() actually generates with.
        const auto chunks =
            sections_for(section_system, work, transcript, budget_chars);
        std::string notes = summarize_sections(work, chunks, req.language,
                                               L("Long recording: summarizing part ",
                                                 "Uzun kayıt: bölüm "), progress);

        // Reduce until the merge actually fits. Every section can emit up to
        // max_tokens_, so enough of them overflow the window on the merge
        // alone -- and the old code only found out inside generate(), after
        // every section had already been paid for. Each pass is the same work
        // one level up, so the notes shrink geometrically; the cap is there
        // because a pass that cannot split is a pass that cannot shrink.
        SummaryRequest final_req = merge_request(work, notes);
        bool merged = false;
        for (int pass = 0; pass < kMaxReducePasses; ++pass) {
            final_req = merge_request(work, notes);
            if (fits(system, build_user_message(final_req), think)) {
                merged = true;
                break;
            }

            const std::size_t before = notes.size();
            const auto groups =
                sections_for(section_system, work, notes, budget_chars);
            if (groups.size() <= 1) break;

            notes = summarize_sections(work, groups, req.language,
                                       L("Condensing the notes: part ",
                                         "Notlar yoğunlaştırılıyor: bölüm "),
                                       progress);
            // A pass is only worth repeating if it actually shrank something.
            // A verbose model summarizing short notes can hand back more than
            // it was given; looping on that burns the user's time to arrive at
            // the same place.
            if (notes.size() >= before) break;
        }
        final_req = merge_request(work, notes);

        // Every way out of that loop except a successful fit lands here with
        // notes that are still too big. Say so, rather than handing them to
        // generate() -- which used to accept anything short of the whole window
        // and then stop mid-sentence at the boundary, saving a half-written
        // summary to summary.txt as though it were finished.
        if (!merged && !fits(system, build_user_message(final_req), think)) {
            throw SummarizerError(
                L("This recording could not be condensed enough to summarize in "
                  "one pass; this model's window holds ",
                  "Bu kayıt tek seferde özetlenecek kadar yoğunlaştırılamadı; "
                  "bu modelin penceresi ") +
                std::to_string(ctx_max_) +
                L(" tokens. Lower the maximum answer length in Settings, or "
                  "choose a model with a larger context.",
                  " token alıyor. Ayarlar'dan azami yanıt uzunluğunu azaltın "
                  "veya daha geniş bağlamlı bir model seçin."));
        }

        if (progress) {
            progress(L("Merging the section notes…",
                       "Bölüm notları birleştiriliyor…"), -1.0);
        }
        out.text = generate(system, build_user_message(final_req), think, progress,
                            &out.cut_short);
        return out;
    }

private:
    // Whether this pass may reason at all: the user asked for it, and the model
    // is one that knows how. A model with no <think> in its own chat template
    // would only be confused by an allowance it has no use for.
    bool thinking_allowed() const { return want_thinking_ && model_thinks_; }

    // What has to be held back for the model to answer in. Reasoning gets an
    // allowance beside the answer rather than out of it -- that separation is
    // the whole fix, and it starts here, where the room is set aside.
    int reserve_for(bool allow_thinking) const {
        return max_tokens_ + (allow_thinking ? think_budget_for(max_tokens_) : 0);
    }

    // Does this system+user pair fit, rendered exactly as generate() will
    // render it, with room left for the answer? This is the one question every
    // budget decision here asks; asking it about the user message alone is
    // what let oversized prompts through.
    bool fits(const std::string& system, const std::string& user,
              bool allow_thinking) {
        const int rendered = count_tokens(render(system, user, allow_thinking));
        return rendered + reserve_for(allow_thinking) <= ctx_max_;
    }

    // Sections that each fit as the prompt they will actually become. The
    // splitting itself lives in llm/chunking.cpp and is tested there; all this
    // supplies is the "does it fit?" question, which is the only part that
    // needs a loaded model.
    std::vector<std::string> sections_for(const std::string& system,
                                          SummaryRequest shape,
                                          const std::string& text,
                                          std::size_t budget_chars) {
        try {
            return split_to_fit(text, budget_chars,
                                [&](const std::string& piece) {
                                    shape.transcript = piece;
                                    return fits(system, build_user_message(shape),
                                                /*allow_thinking=*/false);
                                });
        } catch (const SectionTooLarge&) {
            throw SummarizerError(
                L("The prompt and context alone do not leave room to summarize "
                  "in this model's ",
                  "Yönerge ve bağlam tek başına bu modelin ") +
                std::to_string(ctx_max_) +
                L("-token window. Shorten the template's prompt or context.",
                  " tokenlik penceresinde özetlemeye yer bırakmıyor. Şablonun "
                  "yönergesini veya bağlamını kısaltın."));
        }
    }

    // Note-taking pass over one list of sections. Shared by the first pass over
    // the transcript and by every reduction of the notes that follows.
    std::string summarize_sections(const SummaryRequest& work,
                                   const std::vector<std::string>& sections,
                                   const std::string& language,
                                   const std::string& label,
                                   const ProgressFn& progress) {
        std::string notes;
        for (std::size_t i = 0; i < sections.size(); ++i) {
            if (abort_.load()) {
                throw SummarizerError(L("Summarizing was cancelled.",
                                        "Özetleme iptal edildi."));
            }
            if (progress) {
                progress(label + std::to_string(i + 1) + "/" +
                             std::to_string(sections.size()) +
                             L("…", " özetleniyor…"),
                         static_cast<double>(i) / static_cast<double>(sections.size()));
            }
            SummaryRequest part = work;
            part.transcript = sections[i];
            // Never with thinking: these are mechanical notes, and reasoning
            // here is paid for once per section. A two-hour recording is where
            // that multiplication used to break the whole run.
            notes += generate(partial_prompt(language), build_user_message(part),
                              /*allow_thinking=*/false, nullptr);
            notes += "\n\n";
        }
        return notes;
    }

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
        resolve_limits();
    }

    // Everything that depends on which model was just loaded: how large a
    // window it can be given, and whether it reasons at all.
    void resolve_limits() {
        const int train = std::max(1024, llama_model_n_ctx_train(model_));

        if (ctx_setting_ > 0) {
            ctx_max_ = ctx_setting_;   // typed by hand; honoured as typed
        } else {
            int want = std::min(train, kAutoCtxCeiling);
            want = std::min(want, affordable_ctx());
            // Never over what the model was trained for, and never under the
            // floor unless the model itself is smaller than it.
            ctx_max_ = std::clamp(want, std::min(train, kAutoCtxFloor), train);
        }

        // Prove the ceiling now rather than discovering halfway through a
        // two-hour recording that the cache will not allocate. The probe is
        // freed immediately; every generation makes its own context anyway,
        // sized to that one call.
        ctx_max_ = probe_ctx(ctx_max_);

        // A reasoning model is one whose own chat template knows the tag. That
        // is the same signal llama.cpp's server uses, and it beats matching on
        // file names: a GGUF's name says whatever whoever quantized it typed.
        const char* tmpl = llama_model_chat_template(model_, /*name=*/nullptr);
        model_thinks_ = tmpl != nullptr && contains_ci(tmpl, "think");
    }

    // How large a KV cache this machine can hold, from the shape of the model
    // and what memory is left now that its weights are resident. Rough on
    // purpose -- f16 keys and values, no compute buffers -- because probe_ctx()
    // is what actually settles the question.
    int affordable_ctx() const {
        const int n_layer   = llama_model_n_layer(model_);
        const int n_head    = llama_model_n_head(model_);
        const int n_head_kv = std::max(1, llama_model_n_head_kv(model_));
        const int n_embd    = llama_model_n_embd(model_);
        if (n_layer <= 0 || n_head <= 0 || n_embd <= 0) return kAutoCtxCeiling;

        const long long head_dim = n_embd / n_head;
        const long long per_token =
            2LL * n_layer * head_dim * n_head_kv * 2LL;   // K and V, two bytes each
        if (per_token <= 0) return kAutoCtxCeiling;

        long long budget = kHostKvBudget;
        if (n_gpu_layers_ > 0 && gpu_registry_index_ >= 0 &&
            static_cast<std::size_t>(gpu_registry_index_) < ggml_backend_dev_count()) {
            std::size_t free_bytes = 0, total = 0;
            ggml_backend_dev_memory(
                ggml_backend_dev_get(static_cast<std::size_t>(gpu_registry_index_)),
                &free_bytes, &total);
            // Asked after the weights loaded, so this is what is genuinely
            // spare. A third of it stays behind for compute buffers and for
            // whatever else the driver wants while we are not looking.
            if (free_bytes > 0) {
                budget = static_cast<long long>(free_bytes) / 3 * 2;
            }
        }
        const long long fits_in_budget = budget / per_token;
        return static_cast<int>(
            std::clamp<long long>(fits_in_budget, 1024, kAutoCtxCeiling));
    }

    // The largest context that will actually allocate, at or below `want`.
    int probe_ctx(int want) {
        for (int n = want; n > 1024; n = std::max(1024, n / 2)) {
            if (make_context(n)) {
                free_context();
                return n;
            }
        }
        // Let the real generation report the real failure, with its numbers.
        return 1024;
    }

    void unload_locked() {
        free_context();
        if (model_) {
            llama_model_free(model_);
            model_ = nullptr;
            loaded_path_.clear();
        }
    }

    // A fresh context per generation, so no KV state leaks between summaries --
    // and sized to the call rather than to the ceiling, so summarizing a
    // ten-minute standup does not allocate the cache a two-hour meeting needs.
    bool make_context(int n_ctx) {
        free_context();
        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx       = static_cast<uint32_t>(n_ctx);
        cparams.n_batch     = static_cast<uint32_t>(std::min(kBatch, n_ctx));
        cparams.n_threads   = threads_;
        cparams.n_threads_batch = threads_;
        ctx_ = llama_init_from_model(model_, cparams);
        return ctx_ != nullptr;
    }

    void reset_context(int n_ctx) {
        if (!make_context(n_ctx)) {
            throw SummarizerError(
                L("The LLM context could not be created (",
                  "LLM bağlamı oluşturulamadı (") +
                std::to_string(n_ctx) +
                L(" tokens). There is not enough memory left for it.",
                  " token). Bunun için yeterli bellek kalmadı."));
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

    // The prompt exactly as generate() will feed it, which is what makes fits()
    // an honest measurement.
    //
    // When the model can think but this pass must not, the assistant turn is
    // prefilled with an already-closed <think></think> pair. There is no
    // enable_thinking kwarg to pass instead: llama_chat_apply_template is the
    // legacy renderer and takes no template arguments, so prefilling is the
    // documented way in -- and it costs about five tokens.
    std::string render(const std::string& system, const std::string& user,
                       bool allow_thinking) {
        std::string prompt = apply_chat_template(system, user);
        if (allow_thinking || !model_thinks_) return prompt;

        // Some templates open the block themselves after the assistant header
        // (DeepSeek-R1 does). Closing what is already open beats nesting a
        // second pair inside it.
        prompt += ends_open_think(prompt) ? "\n\n</think>\n\n"
                                          : "<think>\n\n</think>\n\n";
        return prompt;
    }

    // Does the rendered prompt end inside an unclosed <think>?
    static bool ends_open_think(const std::string& prompt) {
        const std::size_t open = prompt.rfind("<think");
        if (open == std::string::npos) return false;
        return prompt.find("</think>", open) == std::string::npos;
    }

    bool decode_one(llama_token id) {
        llama_batch batch = llama_batch_get_one(&id, 1);
        return llama_decode(ctx_, batch) == 0;
    }

    std::string token_to_text(llama_token id) {
        char buf[256];
        const int n = llama_token_to_piece(vocab_, id, buf, sizeof(buf), 0,
                                           /*special=*/false);
        if (n <= 0) return {};
        return std::string(buf, static_cast<std::size_t>(n));
    }

    // `cut_short`, when given, says whether the answer ended on its budget
    // rather than on the model's own end-of-turn.
    std::string generate(const std::string& system, const std::string& user,
                         bool allow_thinking, const ProgressFn& progress,
                         bool* cut_short = nullptr) {
        const std::string prompt = render(system, user, allow_thinking);
        std::vector<llama_token> tokens = tokenize(prompt, /*add_special=*/true);

        const int reserve   = reserve_for(allow_thinking);
        const int max_answer = max_tokens_;
        const int think_budget =
            allow_thinking ? think_budget_for(max_tokens_) : 0;

        // Only as much window as this call needs. The ceiling is what fits()
        // planned against; asking for all of it every time meant a one-page
        // transcript allocated the cache a two-hour meeting needs, on a card
        // that is also holding the weights.
        const long long want =
            static_cast<long long>(tokens.size()) + reserve + kCtxGrain - 1;
        const int n_ctx = static_cast<int>(std::min<long long>(
            ctx_max_, std::max<long long>(1024, want / kCtxGrain * kCtxGrain)));

        if (static_cast<int>(tokens.size()) >= n_ctx) {
            throw SummarizerError(
                L("The text does not fit the context window (",
                  "Metin bağlam penceresine sığmadı (") +
                std::to_string(tokens.size()) + " / " + std::to_string(n_ctx) +
                L(" tokens). Lower the maximum answer length in Settings, or "
                  "choose a model with a larger context.",
                  " token). Ayarlar'dan azami yanıt uzunluğunu azaltın veya "
                  "daha geniş bağlamlı bir model seçin."));
        }

        reset_context(n_ctx);

        if (progress) progress(L("Summarizing…", "Özetleniyor…"), 0.0);

        // Ingest the prompt in batches the context was sized for.
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
        bool hit_window = false;
        bool budget_spent = false;

        // Reasoning spends think_budget, the answer spends max_answer, and
        // neither can eat the other's. An overrun inside <think> is closed for
        // the model rather than left to run the stream out -- which is what
        // "the model used the whole answer budget on reasoning" used to be.
        ReasoningBudget budget(allow_thinking, think_budget, max_answer);
        const double total_budget = think_budget + max_answer;
        int produced = 0;

        for (;;) {
            if (abort_.load()) {
                throw SummarizerError(L("Summarizing was cancelled.",
                                        "Özetleme iptal edildi."));
            }

            llama_token id = llama_sampler_sample(sampler, ctx_, -1);
            if (llama_vocab_is_eog(vocab_, id)) break;

            const std::string piece = token_to_text(id);
            out += piece;
            const ReasoningStep step = budget.feed(piece);
            if (step == ReasoningStep::Stop) budget_spent = true;

            if (progress && (produced++ % 16 == 0)) {
                const double done = budget.think_used() + budget.answer_used();
                progress("", done / total_budget);
            }

            // Reaching the end of the window matters only if there were still
            // tokens owed. fits() permits prompt + reserve == n_ctx exactly, so
            // a model that spends its whole answer budget lands on the boundary
            // with the last token it was ever going to emit -- that is the
            // budget ending, not a truncation, and treating the two alike threw
            // away a complete summary that a one-token-shorter prompt would
            // have kept.
            if (++n_past >= n_ctx) {
                hit_window = (step != ReasoningStep::Stop);
                break;
            }

            if (!decode_one(id)) {
                throw SummarizerError(L("LLM generation broke off (llama_decode).",
                                        "LLM üretimi kesildi (llama_decode)."));
            }

            if (step == ReasoningStep::Stop) break;

            if (step == ReasoningStep::ForceClose) {
                // Reasoning has had its allowance. Hand the model the closing
                // tag and let it write the summary with the answer budget still
                // whole; strip_reasoning() takes the finished block off below.
                static const std::string kClose = "\n</think>\n\n";
                out += kClose;
                std::vector<llama_token> closer = tokenize(kClose, false);
                if (closer.empty()) continue;

                n_past += static_cast<int>(closer.size());
                if (n_past >= n_ctx) { hit_window = true; break; }

                llama_batch batch = llama_batch_get_one(
                    closer.data(), static_cast<int32_t>(closer.size()));
                if (llama_decode(ctx_, batch) != 0) {
                    throw SummarizerError(
                        L("LLM generation broke off (llama_decode).",
                          "LLM üretimi kesildi (llama_decode)."));
                }
            }
        }

        // The weights are the biggest thing on the GPU; drop the KV cache now.
        free_context();
        if (cut_short) *cut_short = budget_spent;

        // The window ran out mid-answer. Callers now budget so this cannot
        // happen, which makes it a last line of defence -- but it used to be
        // the normal way a long summary ended, and it ended silently: the
        // sentence stopped where the context did and the half-written result
        // was saved to summary.txt as though the model had finished.
        if (hit_window) {
            throw SummarizerError(
                L("The summary ran out of context before it finished. Lower the "
                  "maximum answer length in Settings, or choose a model with a "
                  "larger context.",
                  "Özet tamamlanmadan bağlam doldu. Ayarlar'dan azami yanıt "
                  "uzunluğunu azaltın veya daha geniş bağlamlı bir model "
                  "seçin."));
        }

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
                    L("The model produced reasoning but no summary. Turn on "
                      "\"Let the model think first\" in Settings so its "
                      "reasoning gets a budget of its own.",
                      "Model düşünme üretti ama özet üretmedi. Ayarlar'dan "
                      "\"Önce düşünmesine izin ver\" seçeneğini açın; böylece "
                      "düşünmeye kendi bütçesi verilir."));
            }
            throw SummarizerError(L("The LLM returned an empty answer.",
                                    "LLM boş yanıt döndürdü."));
        }
        if (progress) progress("", 1.0);
        return out;
    }

    std::string   model_path_;
    int           ctx_setting_;          // 0 = derive it; see resolve_limits()
    int           ctx_max_ = 4096;       // resolved once the model is loaded
    int           n_gpu_layers_;
    int           gpu_registry_index_;   // -1 = CPU only
    int           threads_;
    int           max_tokens_;
    bool          want_thinking_;        // the setting
    bool          model_thinks_ = false; // ...and whether this GGUF can
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
        std::string ext = paths::to_utf8(it->path().extension());
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
