// Application state: what the UI polls, and the background work behind it.
//
// Recording and heavy processing run off the request thread; the browser polls
// /api/state for phase and progress.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "audio/recorder.h"
#include "audio/sources.h"
#include "config.h"
#include "device.h"
#include "llm/summarizer.h"
#include "pipeline/processor.h"

namespace transcriptor::app {

// Default status line per phase, in the UI's language ("tr" or "en").
std::string phase_message(const std::string& phase, const std::string& lang);

class AppState {
public:
    explicit AppState(Settings settings);
    ~AppState();

    // -- recording --------------------------------------------------------
    // Throws std::runtime_error with a user-facing message.
    void start_recording(const audio::AudioSource& source,
                         const std::optional<audio::AudioSource>& mic_source);
    void stop_and_process();
    void pause_recording();
    void resume_recording();

    // Abort an in-progress recording (no processing) and/or discard the current
    // result, deleting the auto-created session folder.
    void cancel();

    // -- file upload ------------------------------------------------------
    // Decodes then runs the same offline pipeline. Sets the error phase and
    // returns false if the file could not be decoded.
    bool process_file(const paths::fs::path& tmp_path, const std::string& orig_name);

    // -- summarize --------------------------------------------------------
    void start_summarize(const std::string& context, const std::string& template_id);

    // -- settings ---------------------------------------------------------
    Settings settings_copy() const;
    void     replace_settings(const Settings& next);

    // -- snapshots for the API --------------------------------------------
    nlohmann::json state_json() const;
    nlohmann::json result_json() const;

    // Language for user-facing messages. Kept in an atomic rather than read
    // from settings_ so it is safe to call with mutex_ already held, and from
    // the request threads.
    std::string ui_language() const { return ui_en_.load() ? "en" : "tr"; }

    bool            recording() const { return recording_.load(); }
    bool            processing() const { return processing_.load(); }
    std::string     phase() const;
    std::string     message() const;
    paths::fs::path session_dir() const;

    // Backend health, for the settings panel's "fetch models" button.
    std::vector<std::string> list_llm_models(const std::string& base_url_override);

    // -- summarizer model download ----------------------------------------
    // Downloads a catalog GGUF into the models dir in the background and, once
    // it lands, points the settings at it. Returns false and fills `error`
    // when the id is unknown or a download is already running.
    bool start_llm_download(const std::string& model_id, std::string* error);

    // {active, model, message, progress, error} for /api/state.
    nlohmann::json llm_download_json() const;

    // Stop any worker so the process can exit promptly.
    void shutdown();

private:
    void set_phase(const std::string& phase, double progress = -1.0,
                   const std::string& message = "");

    // Picks the string matching ui_language() for a message built in place.
    const char* ui(const char* tr, const char* en) const {
        return ui_en_.load() ? en : tr;
    }

    void begin(std::vector<float> audio, const paths::fs::path& original_file,
               const std::string& original_name);
    void process_worker(std::vector<float> audio);
    void do_summarize();

    bool            any_save() const;
    paths::fs::path ensure_session_dir();
    void            delete_session_dir();
    void            save_transcript();

    void join_worker();
    void join_download();

    mutable std::mutex mutex_;

    Settings   settings_;          // guarded by mutex_
    DeviceInfo device_;            // guarded by mutex_

    std::unique_ptr<pipeline::OfflineProcessor> processor_;   // guarded by mutex_
    std::unique_ptr<llm::Backend>               llm_;         // guarded by mutex_
    std::unique_ptr<audio::Recorder>            recorder_;    // guarded by mutex_

    std::string phase_ = "idle";
    std::string message_;
    double      progress_ = -1.0;

    std::optional<pipeline::ProcessResult> result_;
    std::optional<std::string>             summary_;
    int                                    summary_rev_ = 0;
    paths::fs::path                        session_dir_;

    std::string summary_context_;
    std::string summary_template_;

    // Summarizer model download, guarded by mutex_ except for the flag.
    std::string dl_model_;         // catalog id, "" when never started
    std::string dl_label_;
    std::string dl_message_;
    std::string dl_error_;
    double      dl_progress_ = -1.0;

    std::atomic<bool> ui_en_{false};
    std::atomic<bool> recording_{false};
    std::atomic<bool> processing_{false};
    std::atomic<bool> downloading_{false};
    std::atomic<bool> shutting_down_{false};

    std::thread worker_;
    std::thread download_thread_;
};

}  // namespace transcriptor::app
