// Regression tests for what the HTTP API tells the page (V3).
//
// These run the real AppState behind the real Server, over a real loopback
// socket, and read the answers the way the page does. Only the audio device
// (tests/fake_capture.cpp) and ffmpeg (a shell script on PATH) are stand-ins.
//
// V3: one byte that was not UTF-8 in the status line made every /api/state
// answer 500 until something else changed the phase. The page cannot read a
// 500, so it froze on the previous take's status and never showed the error.
// Two ways in, both from an ffmpeg error message: a 400-byte cut through the
// middle of a character, and text that was never UTF-8 to begin with -- what a
// localized Windows error comes back as.
//
// V7: the template menu saves one field, and the save checked the stored
// device on its behalf -- rewriting a card that was only unplugged to "auto".
//
// V18: /api/state's download report was a hand-kept copy of /api/model/download
// that had lost the "cancelled" field.
//
// V12: a stopped job ends on "idle" like any other, so the page could not tell
// a stopped library re-run from a finished one; and the library's Stop must
// never fall back to discarding the studio's take.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "app/server.h"
#include "app/state.h"
#include "check.h"
#include "fake_capture.h"
#include "util/export.h"
#include "util/paths.h"
#include "util/utf8.h"

using namespace transcriptor;

namespace {

paths::fs::path g_scratch;
int             g_case = 0;

paths::fs::path fresh_dir() {
    const paths::fs::path dir = g_scratch / ("case" + std::to_string(++g_case));
    std::error_code ec;
    paths::fs::create_directories(dir, ec);
    return dir;
}

Settings test_settings(const paths::fs::path& output_dir) {
    Settings s;
    s.output_dir      = paths::to_utf8(output_dir);
    s.samplerate      = 16000;
    s.auto_transcribe = false;   // nothing here should reach a model
    s.auto_summarize  = false;
    s.enable_diarization = false;
    s.device          = "cpu";
    return s;
}

bool valid_utf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2
                    : (c >> 3) == 0x1E ? 3 : -1;
        if (n < 0 || i + n >= s.size()) return false;
        for (int k = 1; k <= n; ++k) {
            if (!utf8::continuation_byte(s[i + k])) return false;
        }
        i += n + 1;
    }
    return true;
}

// An ffmpeg on PATH that runs `script` instead of decoding anything.
struct FakeDecoder {
    paths::fs::path bin;
    std::string     saved_path;

    FakeDecoder(const paths::fs::path& dir, const std::string& script)
        : bin(dir / "bin") {
        std::error_code ec;
        paths::fs::create_directories(bin, ec);
        paths::write_file(bin / "ffmpeg", "#!/bin/sh\n" + script);
        paths::fs::permissions(bin / "ffmpeg", paths::fs::perms::owner_all, ec);
        const char* p = std::getenv("PATH");
        saved_path = p ? p : "";
        setenv("PATH", (paths::to_utf8(bin) + ":" + saved_path).c_str(), 1);
    }
    ~FakeDecoder() { setenv("PATH", saved_path.c_str(), 1); }
};

// An upload the decoder chokes on, then what /api/state says about it.
struct StateAfterFailedDecode {
    int         status = 0;
    std::string message;
};

StateAfterFailedDecode decode_failure(const std::string& ffmpeg_says) {
    const paths::fs::path dir = fresh_dir();
    fake_capture::reset();
    // The message is a printf format, so bytes can be given in octal.
    const FakeDecoder decoder(dir, "printf '" + ffmpeg_says + "'\nexit 1\n");
    const paths::fs::path upload = dir / "broken.m4a";
    paths::write_file(upload, std::string(2048, '\x01'));   // not audio

    app::AppState state(test_settings(dir / "out"));
    app::Server server(&state, "127.0.0.1", 0);
    StateAfterFailedDecode out;
    if (!server.start()) return out;

    state.process_file(upload, "broken.m4a");

    httplib::Client client("127.0.0.1", server.port());
    if (auto res = client.Get("/api/state")) {
        out.status = res->status;
        const auto j = nlohmann::json::parse(res->body, nullptr, false);
        if (j.is_object() && j.contains("message") && j["message"].is_string()) {
            out.message = j["message"].get<std::string>();
        }
    }
    server.stop();
    return out;
}

void test_status_survives_a_split_character() {
    // One ASCII byte, 300 two-byte letters and a newline: 602 bytes, so the
    // last 400 start on the second half of a character.
    std::string format = "E";
    for (int i = 0; i < 300; ++i) format += "ş";
    format += "\\n";
    const StateAfterFailedDecode r = decode_failure(format);
    test::check("V3 /api/state still answers after a message cut mid-character",
                r.status == 200, "HTTP " + std::to_string(r.status));
    test::check("V3 the message is cut on a character boundary",
                valid_utf8(r.message) && r.message.find("ş") != std::string::npos,
                r.message.substr(0, 60));
}

void test_status_survives_text_that_is_not_utf8() {
    // "Erişim engellendi" in Windows-1254, the way a Turkish Windows reports
    // "access denied": 0xFE is the ş, and it is not UTF-8.
    const StateAfterFailedDecode r = decode_failure("Eri\\376im engellendi");
    test::check("V3 /api/state still answers when a message is not UTF-8",
                r.status == 200, "HTTP " + std::to_string(r.status));
    test::check("V3 and the error still reaches the page",
                r.message.find("engellendi") != std::string::npos, r.message);
}

// The page's own token, which every POST has to carry.
std::string page_token(httplib::Client& client) {
    const auto res = client.Get("/");
    if (!res) return {};
    const std::string key = "name=\"csrf-token\" content=\"";
    const auto at = res->body.find(key);
    if (at == std::string::npos) return {};
    const auto from = at + key.size();
    return res->body.substr(from, res->body.find('"', from) - from);
}

void test_a_one_field_save_keeps_a_missing_device() {
    const paths::fs::path dir = fresh_dir();
    Settings s = test_settings(dir / "out");
    s.device = "Vulkan7";   // a card that is not in this machine right now
    app::AppState state(s);
    app::Server server(&state, "127.0.0.1", 0);
    if (!server.start()) {
        test::check("V7 the server starts", false);
        return;
    }
    httplib::Client client("127.0.0.1", server.port());
    const httplib::Headers headers = {{"X-Transcriptor-Token", page_token(client)}};
    const auto res = client.Post("/api/settings", headers,
                                 R"({"summary_template": "standup"})",
                                 "application/json");
    test::check("V7 the template menu's save is accepted", res && res->status == 200,
                res ? res->body : "no answer");
    test::check("V7 it leaves the stored device alone",
                state.settings_copy().device == "Vulkan7",
                "device is now \"" + state.settings_copy().device + "\"");
    test::check("V7 and it saved the template it was sent",
                state.settings_copy().summary_template == "standup");
    server.stop();
}

void test_a_stopped_job_says_so() {
    const paths::fs::path dir = fresh_dir();
    fake_capture::reset();
    // A long decode. exec, so the process a cancel kills is the one holding
    // the pipe; a shell left behind would keep a sleeping child on it.
    const FakeDecoder decoder(dir, "exec sleep 10\n");
    const paths::fs::path upload = dir / "long.m4a";
    paths::write_file(upload, std::string(2048, '\x01'));

    app::AppState state(test_settings(dir / "out"));
    std::thread uploader([&] { state.process_file(upload, "long.m4a"); });
    for (int i = 0; i < 250 && !state.processing(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const bool stopped = state.cancel_job();
    uploader.join();
    const auto st = state.state_json();
    test::check("V12 a stopped job is reported as stopped",
                stopped && st.value("job_cancelled", false) && st["phase"] == "idle",
                "phase " + st.value("phase", std::string()) + ", job_cancelled " +
                    (st.value("job_cancelled", false) ? "true" : "false/absent"));
}

void test_stopping_a_job_never_discards_the_take() {
    const paths::fs::path dir = fresh_dir();
    fake_capture::reset();
    const paths::fs::path wav = dir / "take.wav";
    exporter::save_audio_wav(wav, std::vector<float>(32000, 0.1f), 16000);

    app::AppState state(test_settings(dir / "out"));
    app::Server server(&state, "127.0.0.1", 0);
    if (!server.start() || !state.process_file(wav, "take.wav")) {
        test::check("V12 a take is waiting to be transcribed", false);
        return;
    }
    httplib::Client client("127.0.0.1", server.port());
    const httplib::Headers headers = {{"X-Transcriptor-Token", page_token(client)}};

    // No job is running: the library's Stop arrives just after its run ended.
    auto res = client.Post("/api/cancel", headers, R"({"job_only": true})",
                           "application/json");
    test::check("V12 stopping with no job running stops nothing",
                res && res->body.find("\"none\"") != std::string::npos,
                res ? res->body : "no answer");
    test::check("V12 and the studio's take is still there",
                state.state_json().value("has_audio", false));

    res = client.Post("/api/cancel", headers, "{}", "application/json");
    test::check("the studio's own Cancel still discards the take",
                res && !state.state_json().value("has_audio", true));
    server.stop();
}

void test_both_download_reports_agree() {
    const paths::fs::path dir = fresh_dir();
    app::AppState state(test_settings(dir / "out"));
    const auto in_state = state.state_json()["model_download"];
    const auto own = state.model_download_json();
    std::string keys;
    for (const auto& [k, v] : own.items()) {
        if (!in_state.contains(k)) keys += k + " ";
    }
    test::check("V18 /api/state reports the download with every field of its own",
                keys.empty(), "missing: " + keys);
}

void test_utf8_cuts() {
    const std::string s = "a\xC5\x9F" "b";   // "aşb"
    test::check("V3 a cut from the front backs off to a character boundary",
                utf8::head(s, 2) == "a");
    test::check("V3 a cut from the back moves on to a character boundary",
                utf8::tail(s, 2) == "b");
    test::check("V3 text within the limit is left alone",
                utf8::head(s, 10) == s && utf8::tail(s, 10) == s);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_api <scratch-dir>\n");
        return 2;
    }
    g_scratch = paths::from_utf8(argv[1]);
    std::error_code ec;
    paths::fs::remove_all(g_scratch, ec);
    paths::fs::create_directories(g_scratch, ec);

    // Nothing here may read or write the machine's own settings or models.
    const paths::fs::path home = g_scratch / "home";
    paths::fs::create_directories(home, ec);
    setenv("HOME", paths::to_utf8(home).c_str(), 1);
    setenv("XDG_CONFIG_HOME", paths::to_utf8(home / ".config").c_str(), 1);
    setenv("TRANSCRIPTOR_MODELS_DIR", paths::to_utf8(home / "models").c_str(), 1);

    test_utf8_cuts();
    test_status_survives_a_split_character();
    test_status_survives_text_that_is_not_utf8();
    test_a_one_field_save_keeps_a_missing_device();
    test_a_stopped_job_says_so();
    test_stopping_a_job_never_discards_the_take();
    test_both_download_reports_agree();

    return test::summary("api");
}
