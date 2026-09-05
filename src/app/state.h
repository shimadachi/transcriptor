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
#include "util/lang.h"
#include "util/net.h"

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

    // -- transcribe -------------------------------------------------------
    // Runs the pipeline over the audio that is waiting because
    // settings.auto_transcribe is off. Sets the error phase when nothing waits.
    void start_transcribe();

    // -- summarize --------------------------------------------------------
    void start_summarize(const std::string& context, const std::string& template_id);

    // -- settings ---------------------------------------------------------
    Settings settings_copy() const;
    void     replace_settings(const Settings& next);

    // -- snapshots for the API --------------------------------------------
    nlohmann::json state_json() const;
    nlohmann::json result_json() const;

    // Language for user-facing messages. Read from the process-wide atomic in
    // util/lang.h rather than from settings_, so it is safe to call with mutex_
    // already held, and from the request threads.
    std::string ui_language() const { return lang::english() ? "en" : "tr"; }

    // Drop the "Saved →" pointer when that folder is the one just deleted from
    // the library, so nothing writes into it again.
    void forget_session_dir(const paths::fs::path& dir);

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

    // Stops the download in flight, if any. The curl child is killed and the
    // partial file removed, so the next attempt starts clean. Returns false
    // when nothing was running.
    bool cancel_llm_download();

    // {active, model, message, progress, error} for /api/state.
    nlohmann::json llm_download_json() const;

    // Stop any worker so the process can exit promptly.
    void shutdown();

private:
    void set_phase(const std::string& phase, double progress = -1.0,
                   const std::string& message = "");

    using AudioBuffer = std::shared_ptr<const std::vector<float>>;

    void begin(std::vector<float> audio, const paths::fs::path& original_file,
               const std::string& original_name);
    void process_worker(AudioBuffer audio);
    void do_summarize();

    bool            any_save() const;
    paths::fs::path ensure_session_dir();
    void            delete_session_dir();
    void            save_transcript();

    void join_worker();
    void join_download();

    // The rebuild itself, with mutex_ already held.
    void replace_settings_locked(const Settings& next);

    // Bracket a worker that will hold processor_/llm_ raw pointers. claim_
    // adopts any settings change parked while the last job ran and marks the
    // backends in use; release_ clears that and adopts anything parked since.
    // Both do it under one lock, so a download finishing cannot slip between
    // the check and the rebuild.
    void claim_backends();
    void release_backends();

    mutable std::mutex mutex_;

    Settings   settings_;          // guarded by mutex_
    DeviceInfo device_;            // guarded by mutex_

    // A settings change that landed mid-job. replace_settings() destroys the
    // processor and the summarizer, and a worker holds raw pointers to both --
    // which is why the settings API refuses while a job runs. The download
    // thread has no such gate, so its change waits here instead.
    std::optional<Settings> pending_settings_;   // guarded by mutex_
    bool                    worker_live_ = false;

    std::unique_ptr<pipeline::OfflineProcessor> processor_;   // guarded by mutex_
    std::unique_ptr<llm::Backend>               llm_;         // guarded by mutex_
    std::unique_ptr<audio::Recorder>            recorder_;    // guarded by mutex_

    std::string phase_ = "idle";
    std::string message_;
    double      progress_ = -1.0;

    // The recording (or decoded file) the Transcribe button runs on. Kept after
    // a run so the same audio can be transcribed again — with another model, or
    // with speaker separation switched on.
    AudioBuffer pending_audio_;

    std::optional<pipeline::ProcessResult> result_;
    std::optional<std::string>             summary_;
    // Bumped on every transcript written, so the page can tell a re-run from
    // the text already on screen: has_result never falls back to false in
    // between, which is all a transition-watcher can see.
    int                                    result_rev_ = 0;
    int                                    summary_rev_ = 0;
    paths::fs::path                        session_dir_;

    std::string summary_context_;
    std::string summary_template_;

    // Summarizer model download, guarded by mutex_ except for the flag.
    std::string dl_model_;         // catalog id, "" when never started
    std::string dl_label_;
    std::string dl_message_;
    std::string dl_error_;
    bool        dl_cancelled_ = false;   // stopped on request, not a failure
    double      dl_progress_ = -1.0;

    // Kills the curl behind the catalog download. Reset when one starts, so a
    // cancelled download does not block the next.
    net::Canceller dl_cancel_;

    std::atomic<bool> recording_{false};
    std::atomic<bool> processing_{false};
    std::atomic<bool> downloading_{false};
    std::atomic<bool> shutting_down_{false};

    std::thread worker_;
    std::thread download_thread_;
};

}  // namespace transcriptor::app
