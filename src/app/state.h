// Application state: what the UI polls, and the background work behind it.
//
// Recording and heavy processing run off the request thread; the browser polls
// /api/state for phase and progress.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
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

// "The app is already doing that." Separate from a device or filesystem failure
// so the API can answer 400 (come back in a moment) rather than 500 (something
// is broken): the two read very differently to whoever gets the message.
class BusyError : public std::runtime_error {
public:
    explicit BusyError(const std::string& what) : std::runtime_error(what) {}
};

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

    // Stop the models mid-run. Nothing is discarded: the audio a cancelled
    // transcription was reading, and the transcript a cancelled summary was
    // reading, are still there to try again with. Returns false when no job is
    // running, which is the caller's cue to fall back to cancel() above.
    bool cancel_job();

    // -- file upload ------------------------------------------------------
    // Decodes then runs the same offline pipeline. Sets the error phase and
    // returns false if the file could not be decoded.
    bool process_file(const paths::fs::path& tmp_path, const std::string& orig_name);

    // -- transcribe -------------------------------------------------------
    // Runs the pipeline over the audio that is waiting because
    // settings.auto_transcribe is off. Returns false and fills `error` when
    // nothing waits or a job already holds the worker; the phase is left alone
    // in that case, so a request that loses the race cannot overwrite the
    // status line of the job that won it.
    bool start_transcribe(std::string* error);

    // -- summarize --------------------------------------------------------
    // Same contract as start_transcribe().
    bool start_summarize(const std::string& context, const std::string& template_id,
                         std::string* error);

    // -- library re-runs --------------------------------------------------
    // Run the models again over a recording that is already in the output
    // folder. Neither touches the panels: what they make lands on disk, and
    // the library reloads to show it.
    //
    // `name` says where the result goes — empty replaces the session's
    // original transcript/summary, anything else keeps both by writing
    // transcript.<name>.json / summary.<name>.txt beside it. `source` names
    // which saved transcript to summarize ("" = the original).
    //
    // Same contract as start_transcribe(): false with `error` filled when the
    // recording is gone, the name is unusable, there is nothing to work from,
    // or a job already holds the worker.
    bool start_library_transcribe(const std::string& id, const std::string& name,
                                  std::string* error);
    bool start_library_summarize(const std::string& id, const std::string& source,
                                 const std::string& name, const std::string& context,
                                 const std::string& template_id, std::string* error);

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

    // -- library deletion --------------------------------------------------
    // Remove one session folder, refusing while a job is writing into it.
    //
    // "Writing into it" is not only the studio session: a library re-run works
    // in a folder of its own, which session_dir_ never named, so the old guard
    // let its target be deleted mid-run. What followed was worse than losing
    // the folder — the worker saves through write_file(), which recreates the
    // directories it needs, so the recording, the audio and every other version
    // stayed deleted and a lone transcript reappeared in their place.
    //
    // Serialized against job admission, so neither can slip through the other's
    // window.
    enum class DeleteOutcome { kOk, kBusy, kFailed };
    DeleteOutcome delete_library_session(const paths::fs::path& dir,
                                         const std::string& base);

    bool            recording() const { return recording_.load(); }
    bool            processing() const { return processing_.load(); }
    std::string     phase() const;
    std::string     message() const;
    paths::fs::path session_dir() const;

    // Backend health, for the settings panel's "fetch models" button. Both
    // overrides come from the still-unsaved settings form: the backend has to
    // travel with the URL, or picking "Remote server" and pressing Fetch before
    // saving would list the GGUF files on disk instead of the server's models.
    std::vector<std::string> list_llm_models(const std::string& backend_override,
                                             const std::string& base_url_override);

    // -- model downloads ---------------------------------------------------
    // Downloads a catalog model into the models dir in the background and,
    // once it lands, points the settings at it. `kind` is "llm" (a GGUF for
    // the summarizer) or "whisper" (speech weights). Returns false and fills
    // `error` when the kind or id is unknown, or a download is already
    // running — there is one slot, deliberately: two multi-gigabyte fetches
    // over one connection finish later than the same two in sequence.
    bool start_model_download(const std::string& kind, const std::string& model_id,
                              std::string* error);

    // Stops the download in flight, if any. The curl child is killed and the
    // partial file removed, so the next attempt starts clean. Returns false
    // when nothing was running.
    bool cancel_model_download();

    // {active, kind, model, message, progress, error} for /api/state.
    nlohmann::json model_download_json() const;

    // Stop any worker so the process can exit promptly.
    void shutdown();

private:
    void set_phase(const std::string& phase, double progress = -1.0,
                   const std::string& message = "");

    using AudioBuffer = std::shared_ptr<const std::vector<float>>;

    // `device_error` is non-empty when the capture died mid-take: the audio is
    // still saved and held, but nothing runs on it unasked. `claimed` says the
    // caller already holds the job slot (see claim_job).
    //
    // Returns false when the take was rejected outright -- audio too short to
    // process -- so an upload can answer with the error it just set instead of
    // reporting the rejection as a success.
    bool begin(std::vector<float> audio, const paths::fs::path& original_file,
               const std::string& original_name, const std::string& device_error,
               bool claimed);

    // Write out a take that is being torn down rather than stopped: closing the
    // window during a recording. Honours save_audio, and is the only saving
    // path outside begin().
    void save_orphaned_take(const std::vector<float>& audio);
    void process_worker(AudioBuffer audio);
    void do_summarize();

    // The two model runs, with no opinion about where their output goes, so a
    // library re-run asks for exactly what the live panel asks for.
    pipeline::ProcessResult run_pipeline(const std::vector<float>& audio,
                                         const Settings& settings);
    llm::SummaryRequest build_summary_request(const std::string& transcript,
                                              const std::string& template_id,
                                              const std::string& extra_context);
    llm::Summary run_summarizer(const llm::SummaryRequest& req, bool manage_vram);

    // Empty (with `error` filled) when the id names nothing in the library.
    paths::fs::path library_dir(const std::string& id, std::string* error);
    void do_library_transcribe(const paths::fs::path& dir,
                               const paths::fs::path& audio_path,
                               const std::string& name);
    void do_library_summarize(const paths::fs::path& dir,
                              const std::string& transcript,
                              const std::string& name,
                              const std::string& context,
                              const std::string& template_id);

    bool            any_save() const;
    paths::fs::path ensure_session_dir();
    void            delete_session_dir();
    void            save_transcript();

    // A summary belongs to the transcript it was made from. Drop it -- from
    // memory and from the folder -- when that transcript is replaced.
    void discard_summary();

    // First failure wins: the folder that could not be created explains every
    // write that follows it, so later errors would only bury it.
    void note_save_error(const std::string& message);
    void clear_save_error();

    // One job at a time. Every path that starts a worker takes this slot first,
    // so the check and the claim cannot be split by another request thread:
    // two callers passing a plain processing() test would both go on to assign
    // worker_, and assigning over a joinable thread calls std::terminate.
    bool claim_job();

    // Record which output folder the job that just claimed the slot writes
    // into. Only the library re-runs need it: everything else works in the
    // studio session, which session_dir_ already names.
    void set_job_dir(const paths::fs::path& dir);

    // Run `body` on the worker thread, releasing the job slot when it returns.
    // Owns the thread handle, so joining the previous worker and installing the
    // next one happen under job_mutex_ and can never interleave.
    void start_job(std::function<void()> body);

    void join_worker();          // takes job_mutex_
    void join_worker_locked();   // caller holds job_mutex_
    void join_download();

    // The rebuild itself, with mutex_ already held.
    void replace_settings_locked(const Settings& next);

    // model_download_json()'s body, with mutex_ already held, so state_json()
    // can report the download under its own lock -- from the same code, so
    // the two answers cannot drift apart.
    nlohmann::json model_download_locked() const;

    // Bracket a worker that will hold processor_/llm_ raw pointers. claim_
    // adopts any settings change parked while the last job ran and marks the
    // backends in use; release_ clears that and adopts anything parked since.
    // Both do it under one lock, so a download finishing cannot slip between
    // the check and the rebuild.
    void claim_backends();
    void release_backends();

    mutable std::mutex mutex_;

    // Guards worker_ alone -- the handle, not the state the job touches. Never
    // taken while mutex_ is held, so the two can never deadlock against each
    // other; a running job takes mutex_ freely without ever wanting this one.
    std::mutex job_mutex_;

    // One recording transition at a time, start to finish. Opening a device is
    // slow, and the recorder used to be published only afterwards: a cancel
    // arriving during that gap found no recorder_ to stop, lowered recording_
    // and reported success, and startup then handed a live capture to an app
    // that believed it was idle -- unstoppable, because every route checks
    // recording() first. Held around the whole of start_recording(), cancel(),
    // the handover in stop_and_process(), and shutdown().
    //
    // Outermost of the three: record_mutex_ -> job_mutex_ -> mutex_. Never
    // taken while either of the others is held.
    std::mutex record_mutex_;

    // Serializes taking on an output folder against deleting one. Both are
    // short; what matters is that they cannot interleave, so a re-run cannot be
    // admitted for a folder that is being deleted, and a folder cannot be
    // deleted in the gap between a re-run being admitted and its target being
    // recorded. Outermost again: output_mutex_ -> job_mutex_ -> mutex_.
    std::mutex output_mutex_;

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
    // summary_ ended on the maximum answer length rather than because the
    // model was done. Travels with it, so the note under a cut summary stays
    // exactly as long as that summary does.
    bool                                   summary_cut_short_ = false;
    // The same, for whatever the last job summarized -- a library re-run
    // included, which never touches summary_. Cleared when a job is admitted.
    bool                                   job_cut_short_ = false;
    // Bumped on every transcript written, so the page can tell a re-run from
    // the text already on screen: has_result never falls back to false in
    // between, which is all a transition-watcher can see.
    int                                    result_rev_ = 0;
    int                                    summary_rev_ = 0;
    paths::fs::path                        session_dir_;

    // Where the running job writes, when that is not the studio session: the
    // library folder a re-run was asked for. Empty whenever no such job holds
    // the slot. Guarded by mutex_.
    paths::fs::path                        job_dir_;

    std::string summary_context_;
    std::string summary_template_;

    // Why the last take could not be written to disk, "" when it could. The
    // pipeline runs either way -- the audio is still in memory -- but a phase
    // of "Ready" with nothing saved is a recording the user will lose on exit,
    // so the page says so.
    std::string save_error_;   // guarded by mutex_

    // Summarizer model download, guarded by mutex_ except for the flag.
    std::string dl_kind_;          // "llm" | "whisper", "" when never started
    std::string dl_model_;         // catalog id, "" when never started
    std::string dl_label_;
    std::string dl_message_;
    std::string dl_error_;
    bool        dl_cancelled_ = false;   // stopped on request, not a failure
    double      dl_progress_ = -1.0;

    // Kills the curl behind the catalog download. Reset when one starts, so a
    // cancelled download does not block the next.
    net::Canceller dl_cancel_;

    // The same, for the decode inside a job: it kills the ffmpeg child and is
    // polled between miniaudio chunks. Cleared in claim_job() alongside the
    // rest of the cancellation state.
    net::Canceller job_cancel_;

    // Bumped by every deliberate end of a take: a new recording, and a cancel.
    // stop_and_process() reads it before closing the device and again before
    // committing, because closing takes long enough for a cancel to run in
    // between -- and a cancel that ran must not be undone by the stop it raced.
    std::atomic<unsigned> lifecycle_gen_{0};

    std::atomic<bool> recording_{false};
    std::atomic<bool> processing_{false};
    std::atomic<bool> downloading_{false};
    std::atomic<bool> shutting_down_{false};
    // Raised by cancel_job() and read by the worker as it unwinds, so a run the
    // user stopped ends as a cancellation rather than as a failure. Cleared
    // when the next job is admitted.
    std::atomic<bool> job_cancelled_{false};

    std::thread worker_;
    std::thread download_thread_;
};

}  // namespace transcriptor::app
