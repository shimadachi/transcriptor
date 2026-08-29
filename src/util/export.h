// Save session artifacts (audio / transcript / summary) to disk, and reveal the
// output folder in the OS file manager. Everything stays local.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "util/paths.h"

namespace transcriptor::exporter {

namespace fs = paths::fs;

// Create a timestamped folder under `base` (e.g. .../2026-08-25_14-03-11).
// Throws std::runtime_error if it cannot be created.
fs::path new_session_dir(const std::string& base);

// Mono float32 in [-1, 1] -> 16-bit PCM WAV.
bool save_audio_wav(const fs::path& path, const std::vector<float>& audio,
                    int samplerate);

bool save_text(const fs::path& path, const std::string& text);
bool save_json(const fs::path& path, const nlohmann::json& obj);

// Best-effort: explorer / open / xdg-open.
bool open_in_file_manager(const fs::path& path);

// Remove a directory tree, but only when it sits inside `base` — guards the
// "cancel discards the auto-created session folder" path.
bool remove_session_dir(const fs::path& dir, const std::string& base);

}  // namespace transcriptor::exporter
