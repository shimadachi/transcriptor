// Regression tests for how settings are adopted and kept (V7, V11).
//
// V7: every settings save rebuilt the transcriber and the summarizer, freeing
// whatever models they had loaded -- including the one-field saves the studio's
// template menu makes on every pick. Settings::same_engines() is the question
// AppState now asks before rebuilding; these pin down what it must and must
// not count.
//
// V11: environment overrides and --port are for one run, but the next save of
// anything wrote them into config.json, where they stayed.

#include <cstdlib>
#include <fstream>
#include <string>

#include "check.h"
#include "config.h"
#include "util/paths.h"

using transcriptor::CustomTemplate;
using transcriptor::Settings;

namespace {

void presentation_leaves_the_engines_alone() {
    const Settings before;
    Settings after = before;
    after.summary_template = "standup";
    after.ui_theme = "dark";
    after.ui_language = "tr";
    after.summary_language = "tr";
    after.output_dir = "/somewhere/else";
    after.save_audio = !before.save_audio;
    after.auto_summarize = !before.auto_summarize;
    after.custom_templates["custom-1"] = CustomTemplate{"Call", "Summarize the call.", ""};
    test::check("V7 a template, theme, language or folder change keeps the engines",
                before.same_engines(after));
}

template <typename Change>
void rebuilds(const char* what, Change change) {
    const Settings before;
    Settings after = before;
    change(after);
    test::check((std::string("V7 changing ") + what + " rebuilds the engines").c_str(),
                !before.same_engines(after));
}

#ifndef _WIN32
// POSIX only: the config directory can be moved out of the machine's own
// through the environment there, and nowhere else.
void run_overrides_are_not_saved(const std::string& scratch) {
    namespace fs = transcriptor::paths::fs;
    setenv("HOME", scratch.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (scratch + "/config").c_str(), 1);
    std::error_code ec;
    fs::create_directories(Settings::config_path().parent_path(), ec);
    std::ofstream(Settings::config_path())
        << R"({"output_dir": "/home/user/Transcriptor", "enable_diarization": true,)"
           R"( "port": 5005})";

    setenv("TRANSCRIPTOR_OUTPUT_DIR", "/tmp/one-off-run", 1);
    setenv("TRANSCRIPTOR_NO_DIARIZE", "1", 1);
    Settings s = Settings::load();
    test::check("the overrides apply to this run",
                s.output_dir == "/tmp/one-off-run" && !s.enable_diarization);
    s.override_for_this_run([](Settings& x) { x.port = 5099; });   // --port 5099
    s.summary_template = "standup";   // the save the template menu makes
    s.save();
    unsetenv("TRANSCRIPTOR_OUTPUT_DIR");
    unsetenv("TRANSCRIPTOR_NO_DIARIZE");

    const Settings next = Settings::load();
    test::check("V11 a one-off TRANSCRIPTOR_OUTPUT_DIR is not saved as the folder",
                next.output_dir == "/home/user/Transcriptor", next.output_dir);
    test::check("V11 a one-off TRANSCRIPTOR_NO_DIARIZE is not saved",
                next.enable_diarization);
    test::check("V11 a one-off --port is not saved", next.port == 5005,
                std::to_string(next.port));
    test::check("V11 what the user did change is saved",
                next.summary_template == "standup");

    // Changing an overridden setting on purpose makes it the user's again.
    setenv("TRANSCRIPTOR_OUTPUT_DIR", "/tmp/one-off-run", 1);
    Settings chosen = Settings::load();
    chosen.output_dir = "/home/user/Meetings";
    chosen.save();
    unsetenv("TRANSCRIPTOR_OUTPUT_DIR");
    test::check("V11 an overridden setting the user then changes is saved",
                Settings::load().output_dir == "/home/user/Meetings",
                Settings::load().output_dir);
}
#endif

}  // namespace

int main(int argc, char** argv) {
    presentation_leaves_the_engines_alone();
    rebuilds("the speech model", [](Settings& s) { s.whisper_model = "small"; });
    rebuilds("the model file", [](Settings& s) { s.whisper_model_path = "/m.bin"; });
    rebuilds("the spoken language", [](Settings& s) { s.language = "tr"; });
    rebuilds("the device", [](Settings& s) { s.device = "cpu"; });
    rebuilds("speaker separation", [](Settings& s) { s.enable_diarization = true; });
    rebuilds("the speaker count", [](Settings& s) { s.num_speakers = 3; });
    rebuilds("the summarizer backend", [](Settings& s) { s.llm_backend = "remote"; });
    rebuilds("the GGUF", [](Settings& s) { s.llm_model_path = "/q.gguf"; });
    rebuilds("the context size", [](Settings& s) { s.llm_ctx = 8192; });
    rebuilds("the server URL", [](Settings& s) { s.llm_base_url = "http://h:1/v1"; });
    rebuilds("thinking", [](Settings& s) { s.llm_thinking = true; });
#ifndef _WIN32
    if (argc > 1) {
        namespace fs = transcriptor::paths::fs;
        std::error_code ec;
        fs::remove_all(argv[1], ec);
        fs::create_directories(argv[1], ec);
        run_overrides_are_not_saved(argv[1]);
    }
#else
    (void)argc;
    (void)argv;
#endif
    return test::summary("settings");
}
