#include "util/paths.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#  include <windows.h>
#  include <shlobj.h>
#endif

namespace transcriptor::paths {

namespace {

#ifdef _WIN32
fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) {
        fs::path p(raw);
        CoTaskMemFree(raw);
        return p;
    }
    return {};
}
#endif

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

}  // namespace

fs::path home() {
#ifdef _WIN32
    fs::path p = known_folder(FOLDERID_Profile);
    if (!p.empty()) return p;
    if (const char* h = env("USERPROFILE")) return fs::path(h);
#else
    if (const char* h = env("HOME")) return fs::path(h);
#endif
    return fs::current_path();
}

fs::path config_dir() {
#ifdef _WIN32
    fs::path base = known_folder(FOLDERID_RoamingAppData);
    if (base.empty()) base = home() / "AppData" / "Roaming";
#elif defined(__APPLE__)
    fs::path base = home() / "Library" / "Application Support";
#else
    fs::path base = env("XDG_CONFIG_HOME") ? fs::path(env("XDG_CONFIG_HOME"))
                                           : home() / ".config";
#endif
    return base / "Transcriptor";
}

fs::path models_dir() {
    if (const char* v = env("TRANSCRIPTOR_MODELS_DIR")) return expand_user(v);
    return config_dir() / "models";
}

fs::path default_output_dir() { return home() / "Transcriptor"; }

fs::path expand_user(const std::string& p) {
    if (p.empty()) return {};
    if (p[0] == '~' && (p.size() == 1 || p[1] == '/' || p[1] == '\\')) {
        return home() / fs::path(p.substr(p.size() > 1 ? 2 : 1));
    }
    return from_utf8(p);
}

std::string to_utf8(const fs::path& p) {
#ifdef _WIN32
    const std::wstring& w = p.native();
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
#else
    return p.string();
#endif
}

fs::path from_utf8(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        w.data(), n);
    return fs::path(w);
#else
    return fs::path(s);
#endif
}

bool read_file(const fs::path& p, std::string* out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

bool write_file(const fs::path& p, const std::string& data) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

}  // namespace transcriptor::paths
