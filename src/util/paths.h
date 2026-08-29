// Per-platform standard directories, plus small filesystem helpers.
#pragma once

#include <filesystem>
#include <string>

namespace transcriptor::paths {

namespace fs = std::filesystem;

fs::path home();

// Settings live here: %APPDATA%\Transcriptor  /  ~/Library/Application Support
// /Transcriptor  /  ~/.config/Transcriptor.
fs::path config_dir();

// Downloaded whisper / diarization / GGUF weights.
fs::path models_dir();

// Default recordings folder.
fs::path default_output_dir();

// Expand a leading "~" and make the path absolute; leaves the rest untouched.
fs::path expand_user(const std::string& p);

// UTF-8 string for a path, on every platform (Windows paths are wchar_t).
std::string to_utf8(const fs::path& p);

fs::path from_utf8(const std::string& s);

bool read_file(const fs::path& p, std::string* out);
bool write_file(const fs::path& p, const std::string& data);

}  // namespace transcriptor::paths
