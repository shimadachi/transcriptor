// The diarization clustering threshold, which follows the transcription
// language rather than being a setting of its own.
//
// What this guards: one constant, 0.5, used to sit in Settings as the threshold
// for every language. Measured with eval/ on the CAM++ embedding model, 0.5
// scores 48.7% diarization error rate on Turkish where 0.80 scores 26.4%, and
// 11.2% on English where 0.65 scores 9.5%. It was the wrong number for both,
// and no value exists that is right for both -- which is why it is derived now
// and not stored.

#include "config.h"

#include <cmath>
#include <string>

#include "check.h"

using transcriptor::Settings;

namespace {

bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

std::string show(float v) { return std::to_string(v); }

void follows_the_language() {
    Settings s;

    s.language = "tr";
    test::check("Turkish resolves to 0.80", near(s.cluster_threshold(), 0.80f),
                show(s.cluster_threshold()));

    s.language = "en";
    test::check("English resolves to 0.65", near(s.cluster_threshold(), 0.65f),
                show(s.cluster_threshold()));

    // "" is the UI's auto-detect: no language to choose from, so the value has
    // to sit between the two rather than favour either.
    s.language = "";
    const float mid = s.cluster_threshold();
    test::check("auto-detect resolves to 0.70", near(mid, 0.70f), show(mid));
    test::check("auto-detect sits between the two language values",
                mid > 0.65f && mid < 0.80f, show(mid));
}

void never_returns_something_unusable() {
    // Whatever else changes, sherpa must never be handed 0: that clusters every
    // embedding into a speaker of its own. An unknown language has to land on
    // the middle value, not on nothing.
    for (const char* lang : {"tr", "en", "", "de", "zz"}) {
        Settings s;
        s.language = lang;
        const float v = s.cluster_threshold();
        test::check("resolved threshold is usable", v > 0.0f && v <= 1.0f,
                    std::string(lang) + " -> " + show(v));
    }
}

void is_not_stored() {
    // The whole point of deriving it: nothing about the threshold survives a
    // save, so an old file cannot pin the app to a number chosen for a model
    // and a language that are no longer in play.
    Settings s;
    s.language = "tr";
    test::check("the threshold is not written to the config file",
                !s.to_json().contains("cluster_threshold"),
                s.to_json().dump().substr(0, 60));

    // Every config written before this change carries the old 0.5. Reading one
    // must not resurrect it.
    Settings back;
    back.from_json(nlohmann::json{{"language", "tr"},
                                  {"cluster_threshold", 0.5}});
    test::check("a stored threshold from an older file is ignored",
                near(back.cluster_threshold(), 0.80f),
                show(back.cluster_threshold()));
}

void survives_a_round_trip() {
    Settings s;
    s.language = "tr";

    Settings back;
    back.from_json(s.to_json());
    test::check("the language survives a save and reload",
                back.language == "tr", back.language);
    test::check("so the threshold comes back the same",
                near(back.cluster_threshold(), 0.80f),
                show(back.cluster_threshold()));
}

}  // namespace

int main() {
    follows_the_language();
    never_returns_something_unusable();
    is_not_stored();
    survives_a_round_trip();
    return test::summary("diar_threshold");
}
