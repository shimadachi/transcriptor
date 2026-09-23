// Regression tests for how settings are adopted (V7).
//
// V7: every settings save rebuilt the transcriber and the summarizer, freeing
// whatever models they had loaded -- including the one-field saves the studio's
// template menu makes on every pick. Settings::same_engines() is the question
// AppState now asks before rebuilding; these pin down what it must and must
// not count.

#include <string>

#include "check.h"
#include "config.h"

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

}  // namespace

int main() {
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
    return test::summary("settings");
}
