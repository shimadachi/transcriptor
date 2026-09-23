// Per-platform standard directories, plus small filesystem helpers.
#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

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

// A file name built from one someone else chose -- an upload's -- that can be
// created on every platform: no directories, no separators or characters a
// file system refuses, no Windows device name, and short enough to leave room
// for a prefix. Letters outside ASCII are kept; they are the name.
std::string safe_filename(const std::string& name);

bool read_file(const fs::path& p, std::string* out);

// Replace a file's contents. The destination is left untouched unless the whole
// write succeeds, so a full disk costs the new content and not the old.
bool write_file(const fs::path& p, const std::string& data);

// The same, for files that are only meaningful as a set. Every one of them is
// written in full before any destination is touched, so the ordinary failure --
// running out of room partway through - leaves all of them as they were.
bool write_files(const std::vector<std::pair<fs::path, std::string>>& files);

}  // namespace transcriptor::paths
