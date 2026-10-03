#include "app/state.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

#include "audio/decode.h"
#include "audio/spool.h"
#include "diarize/diarizer.h"
#include "llm/templates.h"
#include "util/export.h"
#include "util/library.h"
#include "util/models.h"

namespace transcriptor::app {

namespace {

struct PhaseText {
    const char* en;
    const char* tr;
};

const std::map<std::string, PhaseText>& phase_messages() {
    static const std::map<std::string, PhaseText> kMessages = {
        {"idle",        {"Ready",               "Hazır"}},
        {"recording",   {"Recording…",          "Kayıt sürüyor…"}},
        {"ready",       {"Ready to transcribe", "Metne dönüştürmeye hazır"}},
        {"transcribe",  {"Transcribing…",       "Metne dönüştürülüyor…"}},
        {"diarize",     {"Separating speakers…","Konuşmacılar ayrılıyor…"}},
        {"attribute",   {"Matching speakers…",  "Konuşmacılar eşleştiriliyor…"}},
        {"done",        {"Done",                "Tamamlandı"}},
        {"summarizing", {"Summarizing…",        "Özetleniyor…"}},
        {"error",       {"Error",               "Hata"}},
    };
    return kMessages;
}

// How a run the user stopped ends. Not "Error": nothing went wrong, and
// nothing was thrown away — the audio or transcript it was reading is still
// there to try again with.
std::string cancelled_message() {
    return L("Stopped. Nothing was discarded.", "Durduruldu. Hiçbir şey silinmedi.");
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// How a summary ends when the answer limit stopped it. It is kept -- it is
// what the model wrote -- but a plain "Done" over a summary that breaks off
// mid-sentence left the user to find that out by reading to the end, if they
// did, and the saved file looked finished either way.
std::string cut_short_message(int max_tokens) {
    const std::string n = std::to_string(max_tokens);
    return lang::english()
        ? "Done, but the summary reached the maximum answer length (" + n +
              " tokens) and stops mid-sentence. Raise it in Settings → Advanced "
              "and summarize again."
        : "Bitti, ama özet azami yanıt uzunluğuna (" + n + " token) ulaştı ve "
              "cümle ortasında kesildi. Ayarlar → Gelişmiş'ten artırıp yeniden "
              "özetleyin.";
}

}  // namespace

std::string phase_message(const std::string& phase, const std::string& lang) {
    auto it = phase_messages().find(phase);
    if (it == phase_messages().end()) return phase;
    return (lang == "tr") ? it->second.tr : it->second.en;
}

AppState::AppState(Settings settings)
    : settings_(std::move(settings)),
      device_(resolve_device(settings_.device, settings_.compute_type)),
      message_(phase_message("idle", settings_.ui_language)),
      summary_template_(settings_.summary_template) {
    lang::set(settings_.ui_language);
    // A crash during a take leaves its scratch spool behind; nothing else ever
    // will, so this is the only place that has to clear them.
    sweep_spool_scratch();
    processor_ = std::make_unique<pipeline::OfflineProcessor>(settings_, device_);
    llm_ = llm::make_backend(settings_, device_);
    live_stt_ = std::make_unique<stt::LiveWhisper>();
    live_ = std::make_unique<pipeline::LiveTranscriber>();
}

AppState::~AppState() { shutdown(); }

void AppState::shutdown() {
    shutting_down_.store(true);
    // The preview has nothing worth waiting for on the way out.
    live_->stop();

    std::unique_ptr<audio::Recorder> recorder;
    {
        // Behind record_mutex_, so a start_recording() still opening its device
        // finishes first and hands the recorder over here rather than
        // publishing it into an app that has already torn itself down.
        std::lock_guard<std::mutex> lifecycle(record_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (processor_) processor_->request_abort();
            if (llm_) llm_->request_abort();
            recorder = std::move(recorder_);
        }
        recording_.store(false);
    }

    // A take in progress is the one thing here that cannot be made again. This
    // used to stop the recorder and drop the samples it returned: closing the
    // window mid-recording lost the lot, default save_audio and all, because
    // writing only ever happened in begin(). Stop it outside the locks (a dead
    // device can take a moment) and write it out before the teardown below.
    if (recorder) {
        std::vector<float> audio = recorder->stop();
        const paths::fs::path spool = recorder->spool_path();
        const std::string spool_err = recorder->spool_error();
        if (spool.empty()) {
            // Held in memory: the path this took before takes were spooled,
            // and still the one a spool that could not be opened falls back to.
            recorder.reset();
            save_orphaned_take(audio);
        } else if (spool == session_audio_path()) {
            // Already written where save_audio would have put it. This is what
            // the spool buys here: closing the window mid-take now costs the
            // last block rather than a pass over the whole recording.
            recorder.reset();
            if (!spool_err.empty()) note_save_error(spool_err);
        } else {
            // Spooled to scratch, because the audio was not being kept or the
            // session folder could not be made. Read it back and let
            // save_orphaned_take() apply the setting as it always has.
            std::string read_err;
            audio::read_wav_f32(spool, &audio, &read_err);
            recorder->discard();
            recorder.reset();
            save_orphaned_take(audio);
        }
    }

    join_worker();
    live_->join();
    // Kill the download rather than wait it out. This used to join and hope:
    // curl has no transfer timeout, so quitting during a stalled fetch hung
    // the app until it was force-killed. The child dies, the .part file is
    // removed, and the thread returns at once -- which is also the only way
    // the state it writes to is safe to tear down.
    std::lock_guard<std::mutex> dl_lock(download_mutex_);
    dl_cancel_.request();
    join_download();
}

void AppState::join_worker() {
    std::lock_guard<std::mutex> lock(job_mutex_);
    join_worker_locked();
}

void AppState::join_worker_locked() {
    if (worker_.joinable()) worker_.join();
}

bool AppState::claim_job() {
    // Nothing new starts once the app is on its way out: shutdown() joins the
    // worker, and a job admitted after that join would never be waited for.
    if (shutting_down_.load()) return false;
    // A live recording owns the session state a job would clear out from under
    // it. The HTTP routes check this too, but not atomically with the claim, so
    // a request that passed its check just before a recording started could
    // still get here. stop_and_process() lowers the flag before it claims.
    //
    // Claim first, then look, because start_recording() does the same the
    // other way round: each raises its own flag and then reads the other's, so
    // at least one of the two always sees the other and backs off. Looking
    // first and claiming after let both through when they interleaved -- a job
    // running under a take that had just cleared its session, and the take's
    // start then joining that job's thread, holding Record, Stop and Cancel
    // for as long as the transcription ran.
    if (processing_.exchange(true)) return false;
    if (recording_.load()) {
        processing_.store(false);
        return false;
    }

    // The last job's cancellation is cleared here, where the slot is taken --
    // not in start_job(), which for an upload does not run until the file has
    // been decoded. Cancel is accepted from the moment processing_ is up, so
    // clearing it later erased a cancellation the user had already been told
    // was taken: the flag went down, the backends were un-aborted, and the
    // "cancelled" upload carried on into transcription as though nothing had
    // been asked.
    job_cancelled_.store(false);
    job_cancel_.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job_dir_.clear();
        job_cut_short_ = false;
        if (processor_) processor_->reset_abort();
        if (llm_) llm_->reset_abort();
    }
    return true;
}

void AppState::set_job_dir(const paths::fs::path& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    job_dir_ = dir;
}

void AppState::start_job(std::function<void()> body) {
    std::lock_guard<std::mutex> lock(job_mutex_);
    // The previous job lowered processing_ before its thread finished unwinding,
    // so there can still be one to join -- but only ever one, and only ours:
    // claim_job() let exactly one caller get here.
    join_worker_locked();
    claim_backends();

    // The last job's cancellation was cleared in claim_job(), before whatever
    // this caller did between taking the slot and getting here. Clearing it
    // again would undo a cancel raised in that window -- and clearing it inside
    // the processor and the backends as they start, which is where it lived
    // before that, meant a shutdown could be requested and then erased by the
    // very job it was meant to stop, leaving join_worker() to wait out a whole
    // transcription, summary, or gigabyte model download.
    //
    // What does belong here: a shutdown that landed while this job was being
    // admitted. claim_job() checked the flag before it stored, not after.
    if (shutting_down_.load()) {
        std::lock_guard<std::mutex> mlock(mutex_);
        if (processor_) processor_->request_abort();
        if (llm_) llm_->request_abort();
    }
    worker_ = std::thread([this, body = std::move(body)] {
        try {
            body();
        } catch (const std::exception& e) {
            // A run the user stopped is not a failure. The engines report it
            // by throwing, the same as anything else, so the flag is what
            // tells the two apart -- otherwise pressing Cancel left a red
            // "Summarizing was cancelled." sitting there as though something
            // had gone wrong.
            if (job_cancelled_.load()) set_phase("idle", -1.0, cancelled_message());
            else set_phase("error", -1.0, e.what());
        }
        // A stop the body caught for itself -- the summarizers report being
        // cancelled as an error of their own -- still ends as a stop, not a
        // failure. But a job that ran to the end while the cancel was in
        // flight has already written its result, and "done" is what
        // happened: this used to report that as stopped too, over a summary
        // sitting on screen and on disk.
        if (job_cancelled_.load()) {
            const std::string ended = phase();
            if (ended != "idle" && ended != "done") {
                set_phase("idle", -1.0, cancelled_message());
            }
        }
        // Nothing is writing into that folder any more, so the library is free
        // to delete it again.
        {
            std::lock_guard<std::mutex> mlock(mutex_);
            job_dir_.clear();
        }
        release_backends();
        processing_.store(false);
    });
}

void AppState::join_download() {
    if (download_thread_.joinable()) download_thread_.join();
}

void AppState::set_phase(const std::string& phase, double progress,
                         const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    phase_ = phase;
    progress_ = progress;
    message_ = message.empty() ? phase_message(phase, ui_language()) : message;
}

std::string AppState::phase() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return phase_;
}

std::string AppState::message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return message_;
}

paths::fs::path AppState::session_dir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_dir_;
}

void AppState::forget_session_dir(const paths::fs::path& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dir.empty() && session_dir_ == dir) session_dir_.clear();
}

AppState::DeleteOutcome AppState::delete_library_session(const paths::fs::path& dir,
                                                         const std::string& base) {
    // Held across the check and the removal both, and taken by library job
    // admission as well, so the two cannot interleave.
    std::lock_guard<std::mutex> guard(output_mutex_);

    if (!dir.empty() && processing_.load()) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Both folders a job can be writing into: the studio's session, and a
        // library re-run's target. Asking only about the first was the whole
        // hole -- a re-run's folder is never session_dir_, so every one of them
        // was deletable while its job ran.
        if (session_dir_ == dir || job_dir_ == dir) return DeleteOutcome::kBusy;
    }
    if (!exporter::remove_session_dir(dir, base)) return DeleteOutcome::kFailed;

    forget_session_dir(dir);
    return DeleteOutcome::kOk;
}

Settings AppState::settings_copy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

void AppState::replace_settings(const Settings& next) {
    std::lock_guard<std::mutex> lock(mutex_);
    // A worker is running against the processor and the summarizer this would
    // delete. The settings API refuses to get here at all while a job runs, but
    // a finished download comes in on its own thread with no such gate, so park
    // the change; the job adopts it on its way out.
    if (worker_live_) {
        pending_settings_ = next;
        return;
    }
    replace_settings_locked(next);
}

void AppState::replace_settings_locked(const Settings& next) {
    const std::string old_lang = ui_language();
    // Rebuilt only when something they read has changed. Replacing them
    // freed the Whisper model and the GGUF they had loaded, and most saves are
    // nothing to do with either: the studio's template menu saves on every
    // pick, so summarizing a transcript twice with two templates read the
    // summarizer back off disk in between.
    const bool rebuild = !settings_.same_engines(next);
    settings_ = next;
    lang::set(next.ui_language);
    // An idle status line is a phase default, so re-render it in the new
    // language; a real message (an error, a filename) is left alone.
    if (message_ == phase_message(phase_, old_lang)) {
        message_ = phase_message(phase_, ui_language());
    }
    summary_template_ = next.summary_template;
    if (!rebuild) return;
    device_ = resolve_device(next.device, next.compute_type);
    // Rebuild lazily: the new device/model only takes effect on the next run.
    processor_ = std::make_unique<pipeline::OfflineProcessor>(settings_, device_);
    llm_ = llm::make_backend(settings_, device_);
}

void AppState::claim_backends() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Adopt before the worker starts, never during: this is the one moment the
    // backends are known to be idle, since every caller has just joined the
    // previous worker.
    if (pending_settings_) {
        replace_settings_locked(*pending_settings_);
        pending_settings_.reset();
    }
    worker_live_ = true;
}

void AppState::release_backends() {
    std::lock_guard<std::mutex> lock(mutex_);
    worker_live_ = false;
    // A download that finished mid-job parked its change; take it now so the
    // model it fetched is live without waiting for another run.
    if (pending_settings_) {
        replace_settings_locked(*pending_settings_);
        pending_settings_.reset();
    }
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

void AppState::start_recording(const audio::AudioSource& source,
                               const std::optional<audio::AudioSource>& mic_source) {
    // The whole transition, start to finish, is one critical section. Opening a
    // device is slow enough that a cancel used to slip through the middle of it
    // -- see record_mutex_ -- and come out the other side with the app idle and
    // a live capture nobody could reach.
    std::lock_guard<std::mutex> lifecycle(record_mutex_);

    // Nothing new starts once the app is on its way out. shutdown() has already
    // stopped and saved whatever was recording; a take admitted after that
    // would open a device nobody is left to close.
    if (shutting_down_.load()) {
        throw BusyError(L("The app is closing.", "Uygulama kapanıyor."));
    }

    // Claim the recorder before anything else. Two starts landing together
    // would otherwise both build a Recorder, both join the worker, and the
    // second would drop the first's device on the floor mid-take.
    if (recording_.exchange(true)) {
        throw BusyError(L("Already recording.", "Zaten kayıtta."));
    }
    // A job still holds the pipeline; the recording it is working on must not
    // be cleared out from under it. Read only after recording_ is raised:
    // claim_job() mirrors this order, and the pair is what keeps the two out
    // of each other's way.
    if (processing_.load()) {
        recording_.store(false);
        throw BusyError(L("A job is already running.", "İşlem sürüyor."));
    }

    join_worker();

    std::unique_ptr<audio::Recorder> recorder;
    bool live = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recorder = std::make_unique<audio::Recorder>(
            source, settings_.samplerate, mic_source, settings_.system_gain,
            settings_.mic_gain);
        result_.reset();
        summary_.reset();
        summary_cut_short_ = false;
        // Bump both revisions on the way out. The page watches these to know
        // when to re-read /api/result; without a bump, clearing a result looks
        // exactly like nothing having happened, and the previous session's
        // transcript stays on screen next to this session's recording.
        ++result_rev_;
        ++summary_rev_;
        // The last take goes too. A recording too short to process leaves this
        // buffer untouched, and Transcribe would then run on the take before
        // it -- audio the user believes they replaced.
        pending_audio_.reset();
        // So does the context typed for the last summary. Only the Summarize
        // button writes it, so without this an auto-summary of this session
        // would carry the previous one's title, attendees and notes.
        summary_context_.clear();
        session_dir_.clear();
        save_error_.clear();   // a new take, and a new chance to write it
        live = settings_.live_transcript;
    }

    // A new take, and a new preview: the last one's text goes with its take.
    live_->clear();
    if (live) attach_live(recorder.get());

    // Before start(), which is when the file is opened. An empty destination
    // means nowhere would take it; the recorder then holds the take in memory
    // and the user gets a recording rather than an error.
    const paths::fs::path spool = spool_destination();
    if (!spool.empty()) recorder->spool_to(spool);

    try {
        recorder->start();
    } catch (...) {
        // Hand the claim back before the message reaches the user, or a device
        // that failed to open would leave the app permanently "recording".
        live_->clear();
        recording_.store(false);
        throw;
    }

    // shutdown() waits on record_mutex_, so it cannot have run past us -- but it
    // can have raised the flag while the device was opening. Close what we just
    // opened rather than publish it into an app that is going away.
    if (shutting_down_.load()) {
        recorder->stop();
        recorder->discard();   // nothing will ever read this take
        recorder.reset();
        live_->stop();
        recording_.store(false);
        throw BusyError(L("The app is closing.", "Uygulama kapanıyor."));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        recorder_ = std::move(recorder);
    }
    lifecycle_gen_.fetch_add(1);   // a new take; anything older is history
    set_phase("recording");
}

void AppState::pause_recording() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recorder_ && recording_.load()) recorder_->pause();
}

void AppState::resume_recording() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recorder_ && recording_.load()) recorder_->resume();
}

// ---------------------------------------------------------------------------
// Live transcript
// ---------------------------------------------------------------------------

pipeline::LiveEngine AppState::live_engine_locked() {
    pipeline::LiveEngine engine;
    // No model, no preview -- and no error from here either: the studio
    // already says which model is missing and where to get it, and a take
    // must never fail for want of its preview.
    if (!models::live_whisper_ready(settings_)) return engine;

    stt::LiveWhisper* stt  = live_stt_.get();
    const paths::fs::path model = settings_.live_whisper_model_file();
    const paths::fs::path vad   = models::vad_model_if_present();
    const DeviceInfo device     = device_;
    const std::string language  = settings_.language;
    const int threads           = settings_.stt_threads;

    engine.prepare = [stt, model, device] {
        stt->load(model, device);
        stt->begin_take();
    };
    engine.decode = [stt, vad, language, threads](const std::vector<float>& audio,
                                                  const std::atomic<bool>* abort) {
        return stt->transcribe(audio, language, vad, threads, abort);
    };
    return engine;
}

void AppState::attach_live(audio::Recorder* recorder) {
    pipeline::LiveEngine engine;
    int samplerate = 16000;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        engine = live_engine_locked();
        samplerate = settings_.samplerate;
        // The same handoff a transcription makes: the summarizer's weights go
        // before speech weights come in. Nothing is using them -- no job runs
        // while a take does.
        if (engine.decode && settings_.manage_vram && llm_) llm_->unload();
    }
    if (!engine.decode) return;
    // A session to feed before there is anything feeding it, so the first
    // block the tap hands over has somewhere to go.
    live_->begin(std::move(engine), samplerate);
    pipeline::LiveTranscriber* live = live_.get();
    recorder->set_tap([live](const std::vector<float>& block, std::size_t offset) {
        live->feed(block, offset);
    });
}

void AppState::release_live_model() {
    live_->stop();
    live_->join();
    live_stt_->unload();
}

bool AppState::set_live(bool on, std::string* error) {
    // Behind the take's own lock, so the recorder cannot be stopped or replaced
    // between being found here and being tapped.
    std::lock_guard<std::mutex> lifecycle(record_mutex_);
    if (shutting_down_.load()) {
        if (error) *error = L("The app is closing.", "Uygulama kapanıyor.");
        return false;
    }

    bool config_failed = false;
    audio::Recorder* recorder = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (settings_.live_transcript != on) {
            settings_.live_transcript = on;
            config_failed = !settings_.save();
        }
        // Null once Stop has taken the recorder, which is then finishing the
        // take -- and the preview with it -- on its own.
        if (recording_.load()) recorder = recorder_.get();
    }
    if (config_failed) {
        note_save_error(L("The settings could not be saved to ",
                          "Ayarlar şuraya kaydedilemedi: ") +
                        paths::to_utf8(Settings::config_path()));
    }

    if (recorder) {
        if (on) {
            attach_live(recorder);
        } else {
            recorder->set_tap(nullptr);
            live_->finish();
        }
    }
    return true;
}

nlohmann::json AppState::live_text_json(std::size_t from) const {
    return live_->text_json(from);
}

void AppState::cancel() {
    // Behind the same lock as start_recording(), so a take that is still
    // opening its device is finished and published before this looks for it.
    // Without that, cancel saw no recorder_, cleared recording_ and reported
    // success while the capture it meant to stop was seconds from going live.
    std::lock_guard<std::mutex> lifecycle(record_mutex_);

    // There is nothing left to cancel once the app is closing, and a great deal
    // to lose: shutdown() has just written the take that was in progress into
    // the session folder, and delete_session_dir() below would take it away
    // again. The request can only be a stray one -- the window is gone.
    if (shutting_down_.load()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recording_.load() && recorder_) {
            recorder_->stop();      // discard the audio — do NOT process
            recorder_->discard();   // and the file it was being written to
            recorder_.reset();
        }
        // Its preview goes too: what the take said goes with the take.
        live_->clear();
        result_.reset();
        summary_.reset();
        summary_cut_short_ = false;
        ++result_rev_;
        ++summary_rev_;
        pending_audio_.reset();
        summary_context_.clear();
        save_error_.clear();
    }
    // Announce the end of this take before anything else can commit it. A Stop
    // already past the handover is sitting in the device close right now; the
    // bump is what tells it, when it comes back, that the take it is holding
    // was cancelled and must not be saved or transcribed.
    lifecycle_gen_.fetch_add(1);
    recording_.store(false);
    delete_session_dir();
    set_phase("idle");
}

bool AppState::cancel_job() {
    if (!processing_.load()) return false;
    // Raised before the engines are asked, so the worker cannot finish and
    // report a failure in the window between the two.
    job_cancelled_.store(true);
    // The decode is part of the job too: an upload spends its first minutes
    // here, and until this was wired through, Cancel during that stretch was
    // accepted and then quietly forgotten.
    job_cancel_.request();
    // So is the preview finishing a take's last seconds before it is kept. A
    // preview stopped short is not kept; the audio waits for Transcribe.
    live_->stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Both, not whichever is thought to be running: a job moves between
        // them -- transcribe hands over to summarize when auto_summarize is on
        // -- and asking only one leaves the other to carry on regardless. An
        // abort a backend was not in the middle of costs nothing; start_job()
        // clears it when the next one is admitted.
        if (processor_) processor_->request_abort();
        if (llm_) llm_->request_abort();
    }
    // The worker unwinds on its own; the phase it lands on says so. Waiting for
    // it here would block the request thread through a whisper batch, which can
    // be seconds. Keep whichever phase is running so the spinner does not jump
    // — only the line under it changes.
    //
    // Read and written under one lock, and only over a stage still running.
    // As a separate read and write, the job could finish in between -- its
    // last word already written -- and "Stopping…" then stood over an idle app
    // until something else moved the phase.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ != "idle" && phase_ != "done" && phase_ != "error" &&
            phase_ != "ready") {
            progress_ = -1.0;
            message_  = L("Stopping…", "Durduruluyor…");
        }
    }
    return true;
}

void AppState::stop_and_process() {
    std::unique_ptr<audio::Recorder> recorder;
    unsigned gen = 0;
    {
        // Only the handover is serialized against the other transitions: the
        // saving and transcribing below take their time, and the job slot
        // claimed further down is what keeps a second one out from there.
        std::lock_guard<std::mutex> lifecycle(record_mutex_);
        gen = lifecycle_gen_.load();
        std::lock_guard<std::mutex> lock(mutex_);
        recorder = std::move(recorder_);
    }
    if (!recorder) return;

    std::vector<float> audio = recorder->stop();
    const std::string err = recorder->error();
    // Spooled, stop() returns nothing and the take is the file; see below.
    const paths::fs::path spool = recorder->spool_path();
    const std::string spool_err = recorder->spool_error();
    // stop() has handed the tail to the preview through its tap; let it finish
    // the last few seconds on its own thread. A cancel that ran underneath us
    // has already cleared it, and this then finds nothing to finish.
    live_->finish();

    {
        // Closing a device is slow, and Cancel can run the whole way through
        // while it happens: it finds no recorder_ (this call took it), clears
        // the session, deletes the folder and leaves the app idle. Committing
        // the take now would put a cancelled recording back on screen and write
        // it to disk. The generation is what tells the two apart.
        std::lock_guard<std::mutex> lifecycle(record_mutex_);
        if (lifecycle_gen_.load() != gen) {
            // Cancelled underneath us. cancel() deleted the session folder,
            // which takes a take spooled into it -- but a scratch spool sits
            // outside that folder and would survive, so it goes from here.
            recorder->discard();
            return;
        }
        recording_.store(false);
    }

    // The take is on disk and has to come back to be transcribed. read_wav_f32
    // sizes the buffer once, from the file, so this costs one allocation of
    // exactly the recording's length -- not the doubling that made a long take
    // freeze the machine in the first place.
    const bool saved_in_place = !spool.empty() && spool == session_audio_path();
    if (!spool.empty()) {
        std::string read_err;
        if (audio::read_wav_f32(spool, &audio, &read_err)) {
            // A scratch spool has done its job once the audio is back: the
            // user either asked for no copy or has one coming in the session
            // folder, and either way this file is not it.
            if (!saved_in_place) {
                std::error_code ec;
                paths::fs::remove(spool, ec);
            }
        } else {
            note_save_error(read_err);
        }
    }
    // Whatever went wrong writing it is the user's to know: the take they are
    // about to see may be shorter than the one they recorded.
    if (!spool_err.empty()) note_save_error(spool_err);
    recorder.reset();

    // A device that dies mid-take -- an unplugged headset, a sink that went
    // away -- still leaves everything captured before it did, and stop()
    // returns all of it. Reporting the error and returning used to drop that
    // vector on the floor: an hour of meeting lost to the last second of it.
    // Hand it to begin() instead, which saves it and holds it for Transcribe;
    // the error travels with it and becomes the status line.
    const bool claimed = claim_job();
    begin(std::move(audio), {}, {}, err, claimed, saved_in_place);
}

bool AppState::process_file(const paths::fs::path& tmp_path,
                            const std::string& orig_name) {
    // Claim the job before the decode rather than after it. A long file takes
    // its time in decode_file(), and until this flag is up the API sees an idle
    // app: a second upload, or a recording, starts on top of the session state
    // this call is about to clear. The server's own check is what tells the
    // user; this one is the guarantee, so it leaves the phase alone.
    if (!claim_job()) return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        result_.reset();
        summary_.reset();
        summary_cut_short_ = false;
        ++result_rev_;   // see start_recording(): a cleared panel must clear
        ++summary_rev_;
        pending_audio_.reset();
        summary_context_.clear();
        session_dir_.clear();
        save_error_.clear();
    }
    live_->clear();   // the last take's preview is not this file's
    set_phase("transcribe", -1.0, L("Decoding the file…", "Dosya çözülüyor…"));

    std::vector<float> audio;
    try {
        audio = audio::decode_file(tmp_path, settings_copy().samplerate, &job_cancel_);
    } catch (const std::exception& e) {
        // A decode the user stopped is not a decode that failed, and must not
        // be reported as one.
        if (job_cancelled_.load()) {
            set_phase("idle", -1.0, cancelled_message());
        } else {
            set_phase("error", -1.0,
                      std::string(L("Could not decode the file: ",
                                    "Dosya çözülemedi: ")) + e.what());
        }
        processing_.store(false);
        return false;
    }
    // Cancel can also land between the decoder's last chunk and here, and
    // begin() below would take that as permission to start transcribing.
    if (job_cancelled_.load()) {
        set_phase("idle", -1.0, cancelled_message());
        processing_.store(false);
        return false;
    }
    // The upload's answer is begin()'s verdict, not "the decode worked". A take
    // under half a second sets the error phase and runs nothing, and returning
    // true regardless had the endpoint reply {ok:true} to a rejected file --
    // the page then waited for a transcript that was never coming.
    return begin(std::move(audio), tmp_path, orig_name, {}, /*claimed=*/true);
}

bool AppState::begin(std::vector<float> audio, const paths::fs::path& original_file,
                     const std::string& original_name,
                     const std::string& device_error, bool claimed,
                     bool already_saved) {
    const Settings settings = settings_copy();
    // Whatever went wrong with the device outranks anything below it: it is
    // both the cause and the thing the user has to act on.
    const std::string device_msg =
        device_error.empty() ? std::string()
                             : L("Audio error: ", "Ses hatası: ") + device_error;

    if (audio.size() < static_cast<std::size_t>(settings.samplerate / 2)) {
        set_phase("error", -1.0,
                  device_msg.empty()
                      ? std::string(L("The audio is too short or empty.",
                                      "Çok kısa/boş ses."))
                      : device_msg);
        // Nothing will run, so hand the job slot back.
        if (claimed) processing_.store(false);
        return false;
    }

    const paths::fs::path dir = ensure_session_dir();
    if (!dir.empty() && settings.save_audio && !already_saved) {
        // Every one of these reports failure, and every one of them used to be
        // discarded: a full disk produced a "Saved →" row, a "Ready" phase and
        // an empty folder, and closing the app took the only copy with it.
        bool ok = false;
        if (!original_file.empty()) {
            // Keep the user's original file as-is rather than re-encoding it.
            const std::string name = original_name.empty() ? "audio" : original_name;
            std::error_code ec;
            ok = paths::fs::copy_file(original_file, dir / paths::from_utf8(name),
                                      paths::fs::copy_options::overwrite_existing,
                                      ec) && !ec;
        } else {
            ok = exporter::save_audio_wav(dir / "audio.wav", audio, settings.samplerate);
        }
        if (!ok) {
            note_save_error(L("The audio could not be written to ",
                              "Ses şuraya yazılamadı: ") + paths::to_utf8(dir));
        }
    }

    auto buffer = std::make_shared<const std::vector<float>>(std::move(audio));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_audio_ = buffer;
    }

    // A take the live transcript heard may already have its transcript, and
    // keeping it saves a second pass over the same audio -- whatever
    // auto_transcribe says, since there is no wait left to spare anyone. Not
    // for an upload, which no preview heard, and not after a device failure,
    // whose message has to stay the status line.
    if (claimed && original_file.empty() && device_msg.empty() &&
        settings.keep_live_transcript && live_->heard_take()) {
        start_job([this, buffer] { keep_live_worker(buffer); });
        return true;
    }

    // The audio is saved and held either way; only the pipeline waits. A take
    // that ended on a device failure waits too, whatever auto_transcribe says:
    // running straight on would replace the one message explaining what
    // happened with a progress line.
    if (!settings.auto_transcribe || !device_msg.empty() || !claimed) {
        if (!device_msg.empty()) {
            set_phase("error", -1.0,
                      device_msg + L(" The recording so far was kept — press "
                                     "Transcribe.",
                                     " O ana kadarki kayıt saklandı — Metne "
                                     "Dönüştür'e basın."));
        } else if (!claimed) {
            set_phase("ready", -1.0,
                      L("A job is running; press Transcribe when it finishes.",
                        "İşlem sürüyor; bitince Metne Dönüştür'e basın."));
        } else {
            set_phase("ready");
        }
        if (claimed) processing_.store(false);   // Transcribe takes its own
        return true;
    }

    start_job([this, buffer] { process_worker(buffer); });
    return true;
}

void AppState::save_orphaned_take(const std::vector<float>& audio) {
    const Settings settings = settings_copy();
    if (!settings.save_audio) return;
    if (audio.size() < static_cast<std::size_t>(settings.samplerate / 2)) return;

    const paths::fs::path dir = ensure_session_dir();
    if (dir.empty()) return;   // ensure_session_dir() already recorded why
    if (!exporter::save_audio_wav(dir / "audio.wav", audio, settings.samplerate)) {
        note_save_error(L("The audio could not be written to ",
                          "Ses şuraya yazılamadı: ") + paths::to_utf8(dir));
    }
}

bool AppState::start_transcribe(std::string* error) {
    AudioBuffer audio;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio = pending_audio_;
    }
    if (!audio || audio->empty()) {
        if (error) {
            *error = L("There is no audio to transcribe.",
                       "Metne dönüştürülecek ses yok.");
        }
        return false;
    }
    if (!claim_job()) {
        if (error) *error = L("A job is already running.", "İşlem sürüyor.");
        return false;
    }

    start_job([this, audio] { process_worker(audio); });
    return true;
}

// The transcription itself, with no opinion about where the result goes.
// Shared by a fresh take and by a re-run over a recording already in the
// library, which wants the same models, the same VRAM handoff and the same
// progress reporting, and none of the current-session bookkeeping.
pipeline::ProcessResult AppState::run_pipeline(const std::vector<float>& audio,
                                               const Settings& settings) {
    // VRAM handoff: drop the summarizer's weights before the STT models load.
    if (settings.manage_vram) {
        set_phase("transcribe", -1.0, L("Freeing VRAM (LLM)…", "VRAM boşaltılıyor (LLM)…"));
        release_live_model();
        std::lock_guard<std::mutex> lock(mutex_);
        if (llm_) llm_->unload();
    }

    auto progress = [this](const std::string& phase, double fraction,
                           const std::string& message) {
        set_phase(phase, fraction, message);
    };

    // Safe to hold raw: claim_backends() marked the backends in use, so
    // replace_settings() parks its change instead of deleting this.
    pipeline::OfflineProcessor* processor = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        processor = processor_.get();
    }
    return processor->run(audio, settings.samplerate, progress);
}

void AppState::process_worker(AudioBuffer audio) {
    const Settings settings = settings_copy();
    {
        pipeline::ProcessResult result = run_pipeline(*audio, settings);

        bool has_text = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result_ = std::move(result);
            ++result_rev_;
            has_text = result_->has_text();
        }
        // The summary on screen described the transcript this one just
        // replaced. Re-running the audio with another model or with speakers
        // switched on left the two side by side -- the new text under the old
        // summary, and summary.txt next to a transcript.txt it no longer
        // matches. Drop it; auto_summarize below writes the new one.
        discard_summary();

        save_transcript();
        set_phase("done", 1.0);

        // Only when the transcript has words in it. A take of silence still
        // finishes with a result, and handing that to the summarizer sequences
        // the VRAM, loads a model and thinks about "Speaker 1: " for a while,
        // to end the run on an error over a recording that simply had nothing
        // in it. The phase stays "done", which is what actually happened.
        if (settings.auto_summarize && has_text) do_summarize();
    }
    // The tail -- catch, release_backends(), processing_ -- belongs to
    // start_job(), which owns the thread this runs on.
}

void AppState::keep_live_worker(AudioBuffer audio) {
    const Settings settings = settings_copy();
    // Stop handed the take's last seconds to the preview, which is still
    // decoding them on its own thread. The transcript is not whole until it
    // has; Cancel stops it here like any other job.
    set_phase("transcribe", -1.0,
              L("Finishing the live transcript…", "Canlı metin tamamlanıyor…"));
    live_->join();

    std::string why;
    std::optional<pipeline::ProcessResult> kept =
        live_->kept_transcript(audio->size(), &why);
    if (!kept) {
        // Not the whole take. Saved as though it were, the part it missed
        // would be gone from the transcript with nothing to say so -- so the
        // take goes the way any other does, and the status line says why.
        if (settings.auto_transcribe) {
            process_worker(audio);
            return;
        }
        const std::string note =
            why.empty() ? std::string()
                        : L("The live transcript was not kept: ",
                            "Canlı metin saklanmadı: ") + why + " ";
        set_phase("ready", -1.0,
                  note + L("Press Transcribe for a complete transcript.",
                           "Tam bir metin için Metne Dönüştür'e basın."));
        return;
    }

    bool has_text = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(*kept);
        ++result_rev_;
        has_text = result_->has_text();
    }
    discard_summary();
    save_transcript();
    // Said, because the full model is still a press away and there is a reason
    // to press it: the preview runs a smaller model, and never separates
    // speakers.
    set_phase("done", 1.0,
              settings.enable_diarization
                  ? L("Done — the live transcript was kept, without speakers. "
                      "Transcribe separates them with the full model.",
                      "Tamam — canlı metin konuşmacısız saklandı. Metne Dönüştür "
                      "konuşmacıları tam modelle ayırır.")
                  : L("Done — the live transcript was kept. Transcribe runs the "
                      "full model over the same audio.",
                      "Tamam — canlı metin saklandı. Metne Dönüştür aynı ses "
                      "üzerinde tam modeli çalıştırır."));

    if (settings.auto_summarize && has_text) do_summarize();
}

bool AppState::any_save() const {
    return settings_.save_audio || settings_.save_transcript ||
           settings_.save_summary;
}

void AppState::note_save_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (save_error_.empty()) save_error_ = message;
}

void AppState::clear_save_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    save_error_.clear();
}

namespace {

// Scratch spools live together under the config directory, so sweeping them is
// one listing and never walks anywhere the user keeps things.
paths::fs::path spool_scratch_dir() { return paths::config_dir() / "spool"; }

// Old enough that no instance still running can be writing to it. A second
// window open right now must never have its take deleted out from under it,
// and there is no portable way to ask whether a file is open -- so age is the
// test, and a day of a crashed run's leftovers is the price.
constexpr std::chrono::hours kSpoolScratchKeep{24};

}  // namespace

void AppState::sweep_spool_scratch() {
    std::error_code ec;
    const paths::fs::path dir = spool_scratch_dir();
    if (!paths::fs::is_directory(dir, ec)) return;
    const auto now = paths::fs::file_time_type::clock::now();
    for (const auto& entry : paths::fs::directory_iterator(dir, ec)) {
        if (ec) break;
        std::error_code each;
        if (!entry.is_regular_file(each) || each) continue;
        const auto when = paths::fs::last_write_time(entry.path(), each);
        if (each || now - when < kSpoolScratchKeep) continue;
        paths::fs::remove(entry.path(), each);
    }
}

paths::fs::path AppState::session_audio_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_dir_.empty()) return {};
    return session_dir_ / "audio.wav";
}

paths::fs::path AppState::spool_destination() {
    // The audio is being kept: spool straight into the session folder, under
    // the name save_audio would have written anyway. Stop then costs no second
    // pass over the recording -- it is already where it belongs.
    if (settings_copy().save_audio) {
        const paths::fs::path dir = ensure_session_dir();
        if (!dir.empty()) return dir / "audio.wav";
        // ensure_session_dir() has already told the user why; fall through,
        // because a take with nowhere to live still must not live in memory.
    }

    // Not being kept, or nowhere to keep it. The recording still has to go
    // somewhere other than RAM: the setting is about what the user ends up
    // with, not about how a five-hour take is held while it is made. This file
    // is deleted the moment the audio has been read back.
    std::error_code ec;
    const paths::fs::path dir = spool_scratch_dir();
    paths::fs::create_directories(dir, ec);
    if (ec) return {};
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return dir / ("take-" + std::to_string(ms) + ".wav");
}

paths::fs::path AppState::ensure_session_dir() {
    std::string failure;
    paths::fs::path dir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!session_dir_.empty() || !any_save()) return session_dir_;
        try {
            session_dir_ = exporter::new_session_dir(settings_.output_dir);
        } catch (const std::exception& e) {
            session_dir_.clear();
            // Swallowed, this is the start of a silent data loss: every save
            // below is skipped because the folder is empty, and the phase still
            // reads "Ready". Keep the reason and let the page show it. It
            // already names the folder and the OS error, so pass it straight on.
            failure = e.what();
        }
        dir = session_dir_;
    }
    if (!failure.empty()) note_save_error(failure);
    return dir;
}

void AppState::delete_session_dir() {
    paths::fs::path dir;
    std::string base;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dir = session_dir_;
        base = settings_.output_dir;
        session_dir_.clear();
    }
    if (!dir.empty()) exporter::remove_session_dir(dir, base);
}

void AppState::discard_summary() {
    paths::fs::path dir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!summary_.has_value()) return;
        summary_.reset();
        summary_cut_short_ = false;
        ++summary_rev_;
        dir = session_dir_;
    }
    if (!dir.empty()) {
        std::error_code ec;
        paths::fs::remove(dir / "summary.txt", ec);   // best effort
    }
}

void AppState::save_transcript() {
    const paths::fs::path dir = ensure_session_dir();

    bool failed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (dir.empty() || !result_.has_value() || !settings_.save_transcript) return;

        const std::string lang = settings_.summary_language;
        failed = !exporter::save_transcript(dir / "transcript.txt",
                                            result_->plain_text(lang, true),
                                            dir / "transcript.json",
                                            result_->to_json(lang));
    }
    if (failed) {
        note_save_error(L("The transcript could not be written to ",
                          "Metin şuraya yazılamadı: ") + paths::to_utf8(dir));
    }
}

// ---------------------------------------------------------------------------
// Summarize
// ---------------------------------------------------------------------------

bool AppState::start_summarize(const std::string& context,
                               const std::string& template_id,
                               std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A result with no words in it is the same answer as no result at all,
        // and the same message. The button that starts this is dimmed for both,
        // but the endpoint is reachable without it.
        if (!result_.has_value() || !result_->has_text()) {
            if (error) {
                *error = L("There is no text to summarize.", "Özetlenecek metin yok.");
            }
            return false;
        }
    }
    // Claimed before the settings below are touched: a caller that loses the
    // race must not rewrite the template the running job is summarizing with.
    if (!claim_job()) {
        if (error) *error = L("A job is already running.", "İşlem sürüyor.");
        return false;
    }
    bool config_failed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        summary_context_ = context;
        if (llm::is_template(template_id) ||
            settings_.custom_templates.count(template_id)) {
            summary_template_ = template_id;
            if (settings_.summary_template != template_id) {
                settings_.summary_template = template_id;
                // Remember the chosen template -- and notice when that fails,
                // rather than leaving the user to discover at the next launch
                // that the app forgot. Reported after the lock: note_save_error
                // takes the same mutex, which is not recursive.
                config_failed = !settings_.save();
            }
        }
    }
    if (config_failed) {
        note_save_error(L("The settings could not be saved to ",
                          "Ayarlar şuraya kaydedilemedi: ") +
                        paths::to_utf8(Settings::config_path()));
    }

    start_job([this] { do_summarize(); });
    return true;
}

// The request every summary is built from: the chosen template's prompt and
// its persistent context, plus whatever context was typed for this run.
// Factored out so a re-run over a library recording asks for exactly what the
// live panel asks for, and does not drift from it.
llm::SummaryRequest AppState::build_summary_request(const std::string& transcript,
                                                    const std::string& template_id,
                                                    const std::string& extra_context) {
    llm::SummaryRequest req;
    std::lock_guard<std::mutex> lock(mutex_);

    req.language    = settings_.summary_language;
    req.transcript  = transcript;
    req.template_id = template_id;

    // Per-template edits: prompt override + persistent context, then the
    // context the user typed for this run. A custom template carries its own
    // prompt rather than overriding a built-in one.
    std::string template_context;
    auto custom = settings_.custom_templates.find(template_id);
    if (custom != settings_.custom_templates.end()) {
        req.system_override = trim(custom->second.prompt);
        template_context = trim(custom->second.context);
    } else {
        auto it = settings_.template_overrides.find(template_id);
        if (it != settings_.template_overrides.end()) {
            req.system_override = trim(it->second.prompt);
            template_context = trim(it->second.context);
        }
    }
    std::vector<std::string> parts;
    if (!template_context.empty()) parts.push_back(template_context);
    if (!trim(extra_context).empty()) parts.push_back(trim(extra_context));
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) req.context += "\n";
        req.context += parts[i];
    }
    return req;
}

// Runs the summarizer and hands back finished text. Throws SummarizerError,
// including when the backend is missing or unhealthy, so every caller reports
// a failure the same way.
llm::Summary AppState::run_summarizer(const llm::SummaryRequest& req,
                                     bool manage_vram) {
    // VRAM handoff the other way: free the STT models before the LLM loads.
    if (manage_vram) {
        set_phase("summarizing", -1.0,
                  L("Handing VRAM over (STT→LLM)…", "VRAM devrediliyor (STT→LLM)…"));
        release_live_model();
        std::lock_guard<std::mutex> lock(mutex_);
        if (processor_) processor_->unload();
    }

    llm::Backend* backend = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        backend = llm_.get();
    }
    if (!backend) {
        throw llm::SummarizerError(
            L("The summarizer is not ready.", "Özetleyici hazır değil."));
    }

    const llm::Availability health = backend->available();
    if (!health.ok) throw llm::SummarizerError(health.message);

    auto progress = [this](const std::string& msg, double fraction) {
        set_phase("summarizing", fraction, msg);
    };
    // Strip here rather than in either backend: this is the one point the text
    // passes through on its way to both the UI and the file.
    llm::Summary summary = backend->summarize(req, progress);
    summary.text = llm::strip_reasoning(summary.text);
    return summary;
}

void AppState::do_summarize() {
    llm::SummaryRequest req;
    bool manage_vram = false;
    bool save_summary = false;
    int  max_tokens = 0;

    {
        std::string transcript, template_id, context;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!result_.has_value()) return;
            transcript   = result_->plain_text(settings_.summary_language);
            template_id  = summary_template_;
            context      = summary_context_;
            manage_vram  = settings_.manage_vram;
            save_summary = settings_.save_summary;
            max_tokens   = settings_.llm_max_tokens;
        }
        req = build_summary_request(transcript, template_id, context);
    }

    set_phase("summarizing");

    try {
        const llm::Summary summary = run_summarizer(req, manage_vram);

        paths::fs::path dir;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            summary_ = summary.text;
            summary_cut_short_ = summary.cut_short;
            job_cut_short_ = summary.cut_short;
            ++summary_rev_;
        }
        dir = ensure_session_dir();
        if (!dir.empty() && save_summary &&
            !exporter::save_text(dir / "summary.txt", summary.text)) {
            note_save_error(L("The summary could not be written to ",
                              "Özet şuraya yazılamadı: ") + paths::to_utf8(dir));
        }
        set_phase("done", 1.0, summary.cut_short ? cut_short_message(max_tokens) : "");
    } catch (const llm::SummarizerError& e) {
        set_phase("error", -1.0, e.what());
    }
}

// ---------------------------------------------------------------------------
// Library re-runs
// ---------------------------------------------------------------------------
//
// Both of these work on a folder in the output directory rather than on the
// current take, and neither touches result_ or summary_: the studio panels are
// about the recording in hand, and re-reading an archived one must not replace
// what is on screen. What they produce lands on disk, and the library panel
// reloads the item to show it.

namespace {

// The text to summarize, out of whichever files this variant has. The JSON
// carries a plain_text field written at save time, which is the same string the
// live path feeds the model; the .txt is the fallback, timestamps and all.
std::string transcript_text(const paths::fs::path& dir, const std::string& name) {
    std::string raw;
    if (paths::read_file(library::transcript_json_file(dir, name), &raw)) {
        auto j = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
        if (!j.is_discarded() && j.contains("plain_text") && j["plain_text"].is_string()) {
            return j["plain_text"].get<std::string>();
        }
    }
    if (paths::read_file(library::transcript_txt_file(dir, name), &raw)) return raw;
    return {};
}

}  // namespace

paths::fs::path AppState::library_dir(const std::string& id, std::string* error) {
    const Settings s = settings_copy();
    const paths::fs::path dir = library::resolve(s.output_dir, id);
    if (dir.empty() && error) {
        *error = L("That recording is no longer there.",
                   "Bu kayıt artık yerinde değil.");
    }
    return dir;
}

bool AppState::start_library_transcribe(const std::string& id,
                                        const std::string& name,
                                        std::string* error) {
    // Admission and deletion take the same lock: without it a Delete could pass
    // its "is anything writing here?" check in the gap between this claim and
    // the folder being recorded as the job's, and take the recording out from
    // under a run that then put a transcript back in the empty space.
    std::lock_guard<std::mutex> guard(output_mutex_);

    const paths::fs::path dir = library_dir(id, error);
    if (dir.empty()) return false;

    if (!name.empty() && !library::valid_variant(name)) {
        if (error) {
            *error = L("That name cannot be used for a file.",
                       "Bu ad bir dosya adı olarak kullanılamaz.");
        }
        return false;
    }

    const std::string audio_name = library::find_audio(dir);
    if (audio_name.empty()) {
        if (error) {
            *error = L("That recording has no audio to transcribe again.",
                       "Bu kaydın yeniden metne dönüştürülecek sesi yok.");
        }
        return false;
    }
    if (std::string missing = models::whisper_missing_reason(settings_copy());
        !missing.empty()) {
        if (error) *error = missing;
        return false;
    }
    if (!claim_job()) {
        if (error) *error = L("A job is already running.", "İşlem sürüyor.");
        return false;
    }

    set_job_dir(dir);
    const paths::fs::path audio_path = dir / paths::from_utf8(audio_name);
    start_job([this, dir, audio_path, name] {
        do_library_transcribe(dir, audio_path, name);
    });
    return true;
}

void AppState::do_library_transcribe(const paths::fs::path& dir,
                                     const paths::fs::path& audio_path,
                                     const std::string& name) {
    const Settings settings = settings_copy();

    set_phase("transcribe", -1.0, L("Reading the recording…", "Kayıt okunuyor…"));
    std::vector<float> audio =
        audio::decode_file(audio_path, settings.samplerate, &job_cancel_);

    const pipeline::ProcessResult result = run_pipeline(audio, settings);

    // Written whatever save_transcript says: this run is not a side effect of
    // recording, it is the thing the user asked for.
    const std::string lang = settings.summary_language;
    if (!exporter::save_transcript(library::transcript_txt_file(dir, name),
                                   result.plain_text(lang, true),
                                   library::transcript_json_file(dir, name),
                                   result.to_json(lang))) {
        throw std::runtime_error(L("The transcript could not be written to ",
                                   "Metin şuraya yazılamadı: ") + paths::to_utf8(dir));
    }
    set_phase("done", 1.0);
}

bool AppState::start_library_summarize(const std::string& id,
                                       const std::string& source,
                                       const std::string& name,
                                       const std::string& context,
                                       const std::string& template_id,
                                       std::string* error) {
    std::lock_guard<std::mutex> guard(output_mutex_);   // see the sibling above

    const paths::fs::path dir = library_dir(id, error);
    if (dir.empty()) return false;

    if ((!name.empty() && !library::valid_variant(name)) ||
        (!source.empty() && !library::valid_variant(source))) {
        if (error) {
            *error = L("That name cannot be used for a file.",
                       "Bu ad bir dosya adı olarak kullanılamaz.");
        }
        return false;
    }

    const std::string text = trim(transcript_text(dir, source));
    if (text.empty()) {
        if (error) {
            *error = L("There is no text to summarize.", "Özetlenecek metin yok.");
        }
        return false;
    }
    if (!claim_job()) {
        if (error) *error = L("A job is already running.", "İşlem sürüyor.");
        return false;
    }

    // Resolved before the job so an unknown id falls back the way the live
    // panel does, rather than reaching the backend as a missing prompt.
    std::string tpl = template_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!llm::is_template(tpl) && !settings_.custom_templates.count(tpl)) {
            tpl = summary_template_;
        }
    }

    set_job_dir(dir);
    start_job([this, dir, text, name, context, tpl] {
        do_library_summarize(dir, text, name, context, tpl);
    });
    return true;
}

void AppState::do_library_summarize(const paths::fs::path& dir,
                                    const std::string& transcript,
                                    const std::string& name,
                                    const std::string& context,
                                    const std::string& template_id) {
    const llm::SummaryRequest req =
        build_summary_request(transcript, template_id, context);
    const Settings settings = settings_copy();

    set_phase("summarizing");
    try {
        const llm::Summary summary = run_summarizer(req, settings.manage_vram);
        if (!exporter::save_text(library::summary_file(dir, name), summary.text)) {
            throw llm::SummarizerError(
                L("The summary could not be written to ",
                  "Özet şuraya yazılamadı: ") + paths::to_utf8(dir));
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_cut_short_ = summary.cut_short;
        }
        set_phase("done", 1.0, summary.cut_short
                                   ? cut_short_message(settings.llm_max_tokens) : "");
    } catch (const llm::SummarizerError& e) {
        set_phase("error", -1.0, e.what());
    }
}

std::vector<std::string> AppState::list_llm_models(
    const std::string& backend_override, const std::string& base_url_override) {
    Settings   settings;
    DeviceInfo device;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings = settings_;
        device   = device_;
    }
    // Follow the form, not the saved config. Overriding only the URL meant that
    // picking "Remote server" and pressing Fetch before saving still built the
    // embedded backend: the dropdown filled with the .gguf files on disk, and
    // the server was never contacted at all.
    if (!backend_override.empty()) settings.llm_backend = backend_override;
    if (settings.llm_backend != "remote") settings.llm_backend = "embedded";
    if (!base_url_override.empty()) settings.llm_base_url = base_url_override;

    // A throwaway backend: never disturbs the loaded summarizer.
    auto probe = llm::make_backend(settings, device);
    return probe->list_models();
}

// ---------------------------------------------------------------------------
// Summarizer model download
// ---------------------------------------------------------------------------

bool AppState::start_model_download(const std::string& kind,
                                    const std::string& model_id,
                                    std::string* error) {
    // Resolve to a label and a fetch before anything is claimed, so an unknown
    // id costs nothing and leaves no slot held.
    const models::LlmModelSpec*     llm  = nullptr;
    const models::WhisperModelSpec* stt  = nullptr;
    // The two speaker models are one thing to the person waiting for them, so
    // they are one download here — and they carry no catalog id.
    const bool diarize = (kind == "diarize");
    // The voice detector has no catalog id either. It comes with any speech
    // model, and on its own only from the studio's caution: a transcription
    // never fetches it.
    const bool vad = (kind == "vad");
    // The live transcript's model comes out of the same catalog as the
    // transcript's, and lands in the same place; only the setting it is
    // chosen for differs.
    const bool live = (kind == "live");
    if (kind == "llm") {
        llm = models::llm_spec(model_id);
    } else if (kind == "whisper" || live) {
        stt = models::whisper_catalog_entry(model_id);
    } else if (!diarize && !vad) {
        if (error) *error = L("Unknown model kind: ", "Bilinmeyen model türü: ") + kind;
        return false;
    }
    if (!diarize && !vad && !llm && !stt) {
        if (error) *error = L("Unknown model: ", "Bilinmeyen model: ") + model_id;
        return false;
    }
    if (diarize && models::diarization_ready(settings_copy())) {
        if (error) {
            *error = L("The speaker models are already downloaded.",
                       "Konuşmacı modelleri zaten indirilmiş.");
        }
        return false;
    }
    if (vad && models::vad_ready()) {
        if (error) {
            *error = L("The voice detector is already downloaded.",
                       "Konuşma algılayıcı zaten indirilmiş.");
        }
        return false;
    }
    const std::string label = llm  ? llm->label
                            : stt  ? stt->label
                            : vad  ? L("Voice detector", "Konuşma algılayıcı")
                                   : L("Speaker models", "Konuşmacı modelleri");
    // Built here rather than glued to the label at the end: the speaker pair is
    // plural, and "Speaker models is ready." is what gluing produced.
    const std::string ready = diarize
        ? L("The speaker models are ready.", "Konuşmacı modelleri hazır.")
        : label + L(" is ready.", " hazır.");

    // Held until the new thread is in download_thread_, and taken by shutdown()
    // around its cancel and join, so the two cannot meet in the middle. This
    // had no gate at all: main() shuts the app down before it stops the
    // server, and a download asked for in between cleared the cancellation
    // shutdown() had just raised, then assigned the thread handle while
    // shutdown() was joining it.
    std::lock_guard<std::mutex> dl_lock(download_mutex_);
    if (shutting_down_.load()) {
        if (error) *error = L("The app is closing.", "Uygulama kapanıyor.");
        return false;
    }
    if (downloading_.exchange(true)) {
        if (error) {
            *error = L("A model is already downloading.",
                       "Zaten bir model indiriliyor.");
        }
        return false;
    }

    join_download();   // the previous thread has already finished
    dl_cancel_.reset();   // a cancelled download must not block this one
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dl_kind_     = kind;
        dl_model_    = (diarize || vad) ? kind : model_id;
        dl_label_    = label;
        dl_message_  = lang::english() ? "Downloading " + label + "…"
                                       : label + " indiriliyor…";
        dl_error_.clear();
        dl_cancelled_ = false;
        dl_progress_ = -1.0;
    }

    // Copied, not captured by pointer: the catalogs are static, but the specs
    // are different types and the thread only needs one of them.
    const bool is_llm = llm != nullptr;
    const models::LlmModelSpec     llm_copy = llm ? *llm : models::LlmModelSpec{};
    const models::WhisperModelSpec stt_copy = stt ? *stt : models::WhisperModelSpec{};

    download_thread_ = std::thread([this, is_llm, diarize, vad, live, llm_copy, stt_copy,
                                    ready] {
        auto progress = [this](const std::string& msg, double fraction) {
            std::lock_guard<std::mutex> lock(mutex_);
            dl_message_  = msg;
            dl_progress_ = fraction;
        };

        std::string      err;
        paths::fs::path  file;
        // Nothing here may escape the thread: an exception leaving it is
        // std::terminate, and the app would vanish mid-download with the
        // cause nowhere on screen. It becomes the download's error instead.
        try {
            if (diarize) {
                // Managed paths, so there is nothing to point the settings at
                // afterwards -- the pipeline finds these by name.
                err = models::ensure_diarization_models(settings_copy(), progress,
                                                        &dl_cancel_);
            } else if (vad) {
                err = models::ensure_vad_model(progress, &dl_cancel_);   // same
            } else if (is_llm) {
                err  = models::ensure_llm_model(llm_copy, progress, &dl_cancel_);
                file = models::llm_model_file(llm_copy);
            } else {
                file = models::whisper_model_file(stt_copy);
                err  = models::ensure_whisper_model_file(stt_copy, progress, &dl_cancel_);
            }
        } catch (const std::exception& e) {
            err = L("The download failed: ", "İndirme başarısız: ") +
                  std::string(e.what());
        }
        // The error string is the same shape either way; the flag is what tells
        // the UI to say "cancelled" instead of colouring it as a failure.
        const bool stopped = dl_cancel_.requested();

        Settings next;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dl_error_ = err;
            dl_cancelled_ = stopped;
            if (err.empty()) {
                dl_message_  = ready;
                dl_progress_ = 1.0;
            } else {
                dl_message_.clear();
                dl_progress_ = -1.0;
            }
            next = settings_;
        }

        if (err.empty() && !diarize && !vad) {
            // Point the engine at what we just fetched, so the user does not
            // have to pick it again afterwards.
            if (is_llm) {
                next.llm_model_path = paths::to_utf8(file);
            } else if (live) {
                next.live_whisper_model = stt_copy.id;
            } else {
                next.whisper_model = stt_copy.id;
                // A stale hand-typed path would win over the model just chosen.
                next.whisper_model_path.clear();
            }
            // The model is on disk either way; what can fail here is recording
            // which file to use. Say so instead of letting the next launch come
            // up pointing at nothing.
            if (!next.save()) {
                std::lock_guard<std::mutex> lock(mutex_);
                dl_message_ += L(" (the model path could not be saved — set it "
                                 "in Settings)",
                                 " (model yolu kaydedilemedi — Ayarlar'dan "
                                 "seçin)");
            }
            replace_settings(next);
        }
        downloading_.store(false);
    });
    return true;
}

bool AppState::cancel_model_download() {
    if (!downloading_.load()) return false;
    // Only asks. The download thread notices its child was killed, clears the
    // partial file and lowers `downloading_` on its way out, so the UI keeps
    // polling the same status endpoint and needs no special case.
    dl_cancel_.request();
    return true;
}

nlohmann::json AppState::model_download_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return model_download_locked();
}

nlohmann::json AppState::model_download_locked() const {
    return {
        {"active", downloading_.load()},
        {"kind", dl_kind_},
        {"model", dl_model_.empty() ? nlohmann::json(nullptr)
                                    : nlohmann::json(dl_model_)},
        {"label", dl_label_},
        {"message", dl_message_},
        {"progress", dl_progress_ < 0 ? nlohmann::json(nullptr)
                                      : nlohmann::json(dl_progress_)},
        {"error", dl_error_.empty() ? nlohmann::json(nullptr)
                                    : nlohmann::json(dl_error_)},
        {"cancelled", dl_cancelled_},
    };
}

// ---------------------------------------------------------------------------
// API snapshots
// ---------------------------------------------------------------------------

nlohmann::json AppState::state_json() const {
    std::lock_guard<std::mutex> lock(mutex_);

    const bool is_recording = recording_.load();
    const double elapsed = recorder_ ? recorder_->elapsed() : 0.0;
    const double level   = recorder_ ? recorder_->level() : 0.0;

    // The preview's own state, and what the studio needs to draw the switch:
    // whether it is on, and whether there is a model for it to run.
    nlohmann::json live = live_->status_json();
    live["on"]    = settings_.live_transcript;
    live["model"] = settings_.live_whisper_model;
    live["ready"] = models::live_whisper_ready(settings_);

    return {
        {"recording", is_recording},
        {"paused", recorder_ ? recorder_->paused() : false},
        {"processing", processing_.load()},
        // Whether the last job was stopped on request. A stopped job ends on
        // "idle", like a job that never ran, so without this a page waiting
        // on a library re-run could only report it as done -- and go looking
        // for a version it never wrote.
        {"job_cancelled", job_cancelled_.load()},
        {"phase", phase_},
        {"message", message_},
        {"progress", progress_ < 0 ? nlohmann::json(nullptr)
                                   : nlohmann::json(progress_)},
        {"device", device_.badge()},
        {"cuda", device_.gpu_available},
        {"elapsed", std::round(elapsed * 10.0) / 10.0},
        {"level", std::round(level * 10000.0) / 10000.0},
        {"has_audio", pending_audio_ && !pending_audio_->empty()},
        {"has_result", result_.has_value()},
        {"has_summary", summary_.has_value()},
        // The last job's summary stopped at the maximum answer length. The
        // library reads it to say so about a re-run; the studio has the same
        // fact beside its own summary in /api/result.
        {"summary_cut_short", job_cut_short_},
        {"result_rev", result_rev_},
        {"summary_rev", summary_rev_},
        {"diarization_enabled", settings_.enable_diarization},
        {"diar_supported", diarize::Diarizer::supported()},
        {"diar_cached", models::diarization_ready(settings_)},
        {"stt_cached", models::whisper_ready(settings_)},
        // The studio offers the voice detector while this is false; nothing
        // else will fetch it.
        {"vad_cached", models::vad_ready()},
        {"output_dir", session_dir_.empty()
                           ? nlohmann::json(nullptr)
                           : nlohmann::json(paths::to_utf8(session_dir_))},
        {"save_error", save_error_.empty() ? nlohmann::json(nullptr)
                                           : nlohmann::json(save_error_)},
        {"auto_transcribe", settings_.auto_transcribe},
        {"auto_summarize", settings_.auto_summarize},
        {"summary_template", summary_template_},
        {"llm_backend", settings_.llm_backend},
        // The same object /api/model/download answers with. It used to be a
        // hand-kept copy, and the copy had lost "cancelled": the studio saw a
        // stopped download only as an error.
        {"model_download", model_download_locked()},
        {"live", live},
    };
}

nlohmann::json AppState::result_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        {"result", result_.has_value()
                       ? result_->to_json(settings_.summary_language)
                       : nlohmann::json(nullptr)},
        {"summary_cut_short", summary_.has_value() && summary_cut_short_},
        {"summary", summary_.has_value() ? nlohmann::json(*summary_)
                                         : nlohmann::json(nullptr)},
        {"output_dir", session_dir_.empty()
                           ? nlohmann::json(nullptr)
                           : nlohmann::json(paths::to_utf8(session_dir_))},
    };
}

}  // namespace transcriptor::app
