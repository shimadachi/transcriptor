#include "util/paths.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#  include <windows.h>
#  include <shlobj.h>
#else
#  include <unistd.h>
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

// A name nothing else is writing: two saves of the same file from two threads,
// or from a second copy of the app, must not stage over each other.
fs::path staging_name(const fs::path& p) {
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    static std::atomic<unsigned> seq{0};
    const std::string tag = ".tmp" + std::to_string(pid) + "-" +
                            std::to_string(seq.fetch_add(1));
    return p.parent_path() / from_utf8(to_utf8(p.filename()) + tag);
}

// The bytes, and whether all of them arrived. Close here, and report on the
// closed stream. A write this size usually sits entirely in the stream buffer,
// so good() answers "nothing has gone wrong yet" rather than "the bytes are on
// disk" -- a full disk or a quota only surfaces when the buffer is flushed.
// Leaving that to the destructor threw the error away: the caller was told the
// transcript had been saved and the file was zero bytes long.
bool spill(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    return out.good();
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
    return write_files({{p, data}});
}

// Nothing is written over until everything has been written. The old code
// opened the destination with trunc and only then discovered it could not fill
// it: a full disk reported the failure honestly and had already erased the
// transcript being replaced -- which, for a deliberate overwrite of a named
// version, was the whole point of the file. Staging beside the destination and
// renaming over it keeps the previous content until there is something complete
// to put in its place, and rename is the one step that cannot half-happen.
//
// The list form exists for files that only mean anything together: transcript
// .txt and .json are one result in two shapes, and replacing one of them while
// the other keeps yesterday's text is its own kind of loss.
bool write_files(const std::vector<std::pair<fs::path, std::string>>& files) {
    std::error_code ec;
    std::vector<fs::path> staged;
    staged.reserve(files.size());

    // Every temp goes, whichever way this ends -- including the ones already
    // renamed into place, whose entries are cleared as they go.
    struct Sweep {
        std::vector<fs::path>& paths;
        ~Sweep() {
            std::error_code e;
            for (const fs::path& p : paths) {
                if (!p.empty()) fs::remove(p, e);
            }
        }
    } sweep{staged};

    for (const auto& [path, data] : files) {
        if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
        const fs::path tmp = staging_name(path);
        staged.push_back(tmp);
        if (!spill(tmp, data)) return false;
    }

    for (std::size_t i = 0; i < files.size(); ++i) {
        fs::rename(staged[i], files[i].first, ec);
        if (ec) return false;
        staged[i].clear();   // it is the destination now; do not sweep it away
    }
    return true;
}

}  // namespace transcriptor::paths
