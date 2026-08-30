// Process-wide interface language, mirrored from Settings::ui_language.
//
// Most user-facing strings are produced far from the settings object — the
// audio decoder, the model downloader, the summarizer — and threading a
// language through every call would drown the signatures. They read it from
// here instead, through L("english", "türkçe").
#pragma once

#include <atomic>
#include <string>

namespace transcriptor {

namespace lang {

// English is the default; only an explicit "tr" switches away from it.
inline std::atomic<bool> g_english{true};

inline void set(const std::string& ui_language) {
    g_english.store(ui_language != "tr", std::memory_order_relaxed);
}

inline bool english() { return g_english.load(std::memory_order_relaxed); }

}  // namespace lang

// English first: it is the source language of the UI.
inline const char* L(const char* en, const char* tr) {
    return lang::english() ? en : tr;
}
inline std::string L(const std::string& en, const std::string& tr) {
    return lang::english() ? en : tr;
}

}  // namespace transcriptor
