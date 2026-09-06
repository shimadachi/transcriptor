#include "app/state.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

#include "audio/decode.h"
#include "diarize/diarizer.h"
#include "llm/templates.h"
#include "util/export.h"
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
    return !processing_.exchange(true);
}

void AppState::start_job(std::function<void()> body) {
    std::lock_guard<std::mutex> lock(job_mutex_);
    // The previous job lowered processing_ before its thread finished unwinding,
    // so there can still be one to join -- but only ever one, and only ours:
    // claim_job() let exactly one caller get here.
    join_worker_locked();
    claim_backends();

    // Clear the last job's cancellation here, where the worker does not exist
    // yet, rather than inside the processor and the backends as they start.
    // Doing it there meant a shutdown could be requested and then erased by the
    // job it was meant to stop, leaving join_worker() below to wait out a whole
    // transcription, summary, or gigabyte model download.
    {
        std::lock_guard<std::mutex> mlock(mutex_);
        if (processor_) processor_->reset_abort();
        if (llm_) llm_->reset_abort();
    }
    // ...and if the shutdown landed while this job was being admitted, put it
    // straight back: claim_job() checked the flag before that store, not after.
    if (shutting_down_.load()) {
        std::lock_guard<std::mutex> mlock(mutex_);
        if (processor_) processor_->request_abort();
        if (llm_) llm_->request_abort();
    }
    worker_ = std::thread([this, body = std::move(body)] {
        try {
            body();
        } catch (const std::exception& e) {
            set_phase("error", -1.0, e.what());
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
    settings_ = next;
    lang::set(next.ui_language);
    // An idle status line is a phase default, so re-render it in the new
    // language; a real message (an error, a filename) is left alone.
    if (message_ == phase_message(phase_, old_lang)) {
        message_ = phase_message(phase_, ui_language());
    }
    summary_template_ = next.summary_template;
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
        audio = audio::decode_file(tmp_path, settings_copy().samplerate);
    } catch (const std::exception& e) {
        set_phase("error", -1.0,
                  std::string(L("Could not decode the file: ",
                                "Dosya çözülemedi: ")) + e.what());
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

void AppState::process_worker(AudioBuffer audio) {
    const Settings settings = settings_copy();
    {
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

        pipeline::ProcessResult result;
        {
            // Safe to hold raw: claim_backends() marked the backends in use, so
            // replace_settings() parks its change instead of deleting this.
            pipeline::OfflineProcessor* processor = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                processor = processor_.get();
            }
            result = processor->run(*audio, settings.samplerate, progress);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            result_ = std::move(result);
            ++result_rev_;
        }
        // The summary on screen described the transcript this one just
        // replaced. Re-running the audio with another model or with speakers
        // switched on left the two side by side -- the new text under the old
        // summary, and summary.txt next to a transcript.txt it no longer
        // matches. Drop it; auto_summarize below writes the new one.
        discard_summary();

        save_transcript();
        set_phase("done", 1.0);

        if (settings.auto_summarize) do_summarize();
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
        failed = !exporter::save_text(dir / "transcript.txt",
                                      result_->plain_text(lang, true));
        failed |= !exporter::save_json(dir / "transcript.json",
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
        if (!result_.has_value()) {
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

void AppState::do_summarize() {
    llm::SummaryRequest req;
    bool manage_vram = false;
    bool save_summary = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!result_.has_value()) return;

        req.language    = settings_.summary_language;
        req.transcript  = result_->plain_text(settings_.summary_language);
        req.template_id = summary_template_;

        // Per-template edits: prompt override + persistent context, then the
        // context the user typed for this run. A custom template carries its own
        // prompt rather than overriding a built-in one.
        std::string template_context;
        auto custom = settings_.custom_templates.find(summary_template_);
        if (custom != settings_.custom_templates.end()) {
            req.system_override = trim(custom->second.prompt);
            template_context = trim(custom->second.context);
        } else {
            auto it = settings_.template_overrides.find(summary_template_);
            if (it != settings_.template_overrides.end()) {
                req.system_override = trim(it->second.prompt);
                template_context = trim(it->second.context);
            }
        }
        std::vector<std::string> parts;
        if (!template_context.empty()) parts.push_back(template_context);
        if (!trim(summary_context_).empty()) parts.push_back(trim(summary_context_));
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) req.context += "\n";
            req.context += parts[i];
        }

        manage_vram  = settings_.manage_vram;
        save_summary = settings_.save_summary;
    }

    set_phase("summarizing");

    try {
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
        if (!health.ok) {
            set_phase("error", -1.0, health.message);
            return;
        }

        auto progress = [this](const std::string& msg, double fraction) {
            set_phase("summarizing", fraction, msg);
        };
        // Strip here rather than in either backend: this is the one point the
        // text passes through on its way to both the UI and summary.txt.
        std::string summary = llm::strip_reasoning(backend->summarize(req, progress));

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

bool AppState::start_llm_download(const std::string& model_id, std::string* error) {
    const models::LlmModelSpec* spec = models::llm_spec(model_id);
    if (!spec) {
        if (error) *error = L("Unknown model: ", "Bilinmeyen model: ") + model_id;
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
        dl_model_    = spec->id;
        dl_label_    = spec->label;
        dl_message_  = lang::english() ? "Downloading " + spec->label + "…"
                                       : spec->label + " indiriliyor…";
        dl_error_.clear();
        dl_cancelled_ = false;
        dl_progress_ = -1.0;
    }

    const models::LlmModelSpec copy = *spec;
    download_thread_ = std::thread([this, copy] {
        auto progress = [this](const std::string& msg, double fraction) {
            std::lock_guard<std::mutex> lock(mutex_);
            dl_message_  = msg;
            dl_progress_ = fraction;
        };

        const std::string err = models::ensure_llm_model(copy, progress, &dl_cancel_);
        // The error string is the same shape either way; the flag is what tells
        // the UI to say "cancelled" instead of colouring it as a failure.
        const bool stopped = dl_cancel_.requested();
        const paths::fs::path file = models::llm_model_file(copy);

        Settings next;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dl_error_ = err;
            dl_cancelled_ = stopped;
            if (err.empty()) {
                dl_message_  = copy.label + L(" is ready.", " hazır.");
                dl_progress_ = 1.0;
            } else {
                dl_message_.clear();
                dl_progress_ = -1.0;
            }
            next = settings_;
        }

        if (err.empty()) {
            // Point the embedded backend at what we just fetched, so the user
            // does not have to pick the file by hand afterwards.
            next.llm_model_path = paths::to_utf8(file);
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

bool AppState::cancel_llm_download() {
    if (!downloading_.load()) return false;
    // Only asks. The download thread notices its child was killed, clears the
    // partial file and lowers `downloading_` on its way out, so the UI keeps
    // polling the same status endpoint and needs no special case.
    dl_cancel_.request();
    return true;
}

nlohmann::json AppState::llm_download_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        {"active", downloading_.load()},
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
        // Same fields as llm_download_json(), built here to keep one lock.
        {"llm_download",
         {{"active", downloading_.load()},
          {"model", dl_model_.empty() ? nlohmann::json(nullptr)
                                      : nlohmann::json(dl_model_)},
          {"label", dl_label_},
          {"message", dl_message_},
          {"progress", dl_progress_ < 0 ? nlohmann::json(nullptr)
                                        : nlohmann::json(dl_progress_)},
          {"error", dl_error_.empty() ? nlohmann::json(nullptr)
                                      : nlohmann::json(dl_error_)}}},
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
