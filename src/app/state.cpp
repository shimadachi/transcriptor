#include "app/state.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

#include "audio/decode.h"
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
    processor_ = std::make_unique<pipeline::OfflineProcessor>(settings_, device_);
    llm_ = llm::make_backend(settings_, device_);
}

AppState::~AppState() { shutdown(); }

void AppState::shutdown() {
    shutting_down_.store(true);

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
        recorder.reset();
        save_orphaned_take(audio);
    }

    join_worker();
    // Kill the download rather than wait it out. This used to join and hope:
    // curl has no transfer timeout, so quitting during a stalled fetch hung
    // the app until it was force-killed. The child dies, the .part file is
    // removed, and the thread returns at once -- which is also the only way
    // the state it writes to is safe to tear down.
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
    if (recording_.load()) return false;
    if (processing_.exchange(true)) return false;

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
        // A job that ran to the end while a cancel was in flight has already
        // written its result; say so rather than reporting it as stopped.
        if (job_cancelled_.load() && phase() != "idle") {
            set_phase("idle", -1.0, cancelled_message());
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
    // be cleared out from under it.
    if (processing_.load()) {
        recording_.store(false);
        throw BusyError(L("A job is already running.", "İşlem sürüyor."));
    }

    join_worker();

    std::unique_ptr<audio::Recorder> recorder;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recorder = std::make_unique<audio::Recorder>(
            source, settings_.samplerate, mic_source, settings_.system_gain,
            settings_.mic_gain);
        result_.reset();
        summary_.reset();
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
    }

    try {
        recorder->start();
    } catch (...) {
        // Hand the claim back before the message reaches the user, or a device
        // that failed to open would leave the app permanently "recording".
        recording_.store(false);
        throw;
    }

    // shutdown() waits on record_mutex_, so it cannot have run past us -- but it
    // can have raised the flag while the device was opening. Close what we just
    // opened rather than publish it into an app that is going away.
    if (shutting_down_.load()) {
        recorder->stop();
        recorder.reset();
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
            recorder_->stop();   // discard the audio — do NOT process
            recorder_.reset();
        }
        result_.reset();
        summary_.reset();
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
    set_phase(phase(), -1.0, L("Stopping…", "Durduruluyor…"));
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
    recorder.reset();

    {
        // Closing a device is slow, and Cancel can run the whole way through
        // while it happens: it finds no recorder_ (this call took it), clears
        // the session, deletes the folder and leaves the app idle. Committing
        // the take now would put a cancelled recording back on screen and write
        // it to disk. The generation is what tells the two apart.
        std::lock_guard<std::mutex> lifecycle(record_mutex_);
        if (lifecycle_gen_.load() != gen) return;   // cancelled underneath us
        recording_.store(false);
    }

    // A device that dies mid-take -- an unplugged headset, a sink that went
    // away -- still leaves everything captured before it did, and stop()
    // returns all of it. Reporting the error and returning used to drop that
    // vector on the floor: an hour of meeting lost to the last second of it.
    // Hand it to begin() instead, which saves it and holds it for Transcribe;
    // the error travels with it and becomes the status line.
    const bool claimed = claim_job();
    begin(std::move(audio), {}, {}, err, claimed);
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
        ++result_rev_;   // see start_recording(): a cleared panel must clear
        ++summary_rev_;
        pending_audio_.reset();
        summary_context_.clear();
        session_dir_.clear();
        save_error_.clear();
    }
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
                     const std::string& device_error, bool claimed) {
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
    if (!dir.empty() && settings.save_audio) {
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
std::string AppState::run_summarizer(const llm::SummaryRequest& req,
                                     bool manage_vram) {
    // VRAM handoff the other way: free the STT models before the LLM loads.
    if (manage_vram) {
        set_phase("summarizing", -1.0,
                  L("Handing VRAM over (STT→LLM)…", "VRAM devrediliyor (STT→LLM)…"));
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
    return llm::strip_reasoning(backend->summarize(req, progress));
}

void AppState::do_summarize() {
    llm::SummaryRequest req;
    bool manage_vram = false;
    bool save_summary = false;

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
        }
        req = build_summary_request(transcript, template_id, context);
    }

    set_phase("summarizing");

    try {
        const std::string summary = run_summarizer(req, manage_vram);

        paths::fs::path dir;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            summary_ = summary;
            ++summary_rev_;
        }
        dir = ensure_session_dir();
        if (!dir.empty() && save_summary &&
            !exporter::save_text(dir / "summary.txt", summary)) {
            note_save_error(L("The summary could not be written to ",
                              "Özet şuraya yazılamadı: ") + paths::to_utf8(dir));
        }
        set_phase("done", 1.0);
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
    const bool manage_vram = settings_copy().manage_vram;

    set_phase("summarizing");
    try {
        const std::string summary = run_summarizer(req, manage_vram);
        if (!exporter::save_text(library::summary_file(dir, name), summary)) {
            throw llm::SummarizerError(
                L("The summary could not be written to ",
                  "Özet şuraya yazılamadı: ") + paths::to_utf8(dir));
        }
        set_phase("done", 1.0);
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
    if (kind == "llm") {
        llm = models::llm_spec(model_id);
    } else if (kind == "whisper") {
        stt = models::whisper_catalog_entry(model_id);
    } else if (!diarize) {
        if (error) *error = L("Unknown model kind: ", "Bilinmeyen model türü: ") + kind;
        return false;
    }
    if (!diarize && !llm && !stt) {
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
    const std::string label = llm  ? llm->label
                            : stt  ? stt->label
                                   : L("Speaker models", "Konuşmacı modelleri");
    // Built here rather than glued to the label at the end: the speaker pair is
    // plural, and "Speaker models is ready." is what gluing produced.
    const std::string ready = diarize
        ? L("The speaker models are ready.", "Konuşmacı modelleri hazır.")
        : label + L(" is ready.", " hazır.");

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
        dl_model_    = diarize ? kind : model_id;
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

    download_thread_ = std::thread([this, is_llm, diarize, llm_copy, stt_copy, ready] {
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

        if (err.empty() && !diarize) {
            // Point the engine at what we just fetched, so the user does not
            // have to pick it again afterwards.
            if (is_llm) {
                next.llm_model_path = paths::to_utf8(file);
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
        {"result_rev", result_rev_},
        {"summary_rev", summary_rev_},
        {"diarization_enabled", settings_.enable_diarization},
        {"diar_supported", diarize::Diarizer::supported()},
        {"diar_cached", models::diarization_ready(settings_)},
        {"stt_cached", models::whisper_ready(settings_)},
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
    };
}

nlohmann::json AppState::result_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        {"result", result_.has_value()
                       ? result_->to_json(settings_.summary_language)
                       : nlohmann::json(nullptr)},
        {"summary", summary_.has_value() ? nlohmann::json(*summary_)
                                         : nlohmann::json(nullptr)},
        {"output_dir", session_dir_.empty()
                           ? nlohmann::json(nullptr)
                           : nlohmann::json(paths::to_utf8(session_dir_))},
    };
}

}  // namespace transcriptor::app
