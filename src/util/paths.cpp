#include "util/paths.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "util/utf8.h"

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

std::string safe_filename(const std::string& name) {
    std::string base = name;
    const auto slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);

    // Letters outside ASCII are kept whole. Replacing every byte of them
    // turned "Toplantı kaydı.wav" into "Toplant__ kayd__.wav" -- in an app
    // whose users name their files in Turkish. A byte that is not part of a
    // well-formed character still goes.
    std::string out;
    for (std::size_t i = 0; i < base.size();) {
        const auto c = static_cast<unsigned char>(base[i]);
        if (c >= 0x80) {
            const std::size_t n = (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2
                                : (c >> 3) == 0x1E ? 3 : 0;
            bool whole = n > 0 && i + n < base.size();
            for (std::size_t k = 1; whole && k <= n; ++k) {
                whole = utf8::continuation_byte(base[i + k]);
            }
            if (whole) {
                out.append(base, i, n + 1);
                i += n + 1;
            } else {
                out += '_';
                ++i;
            }
            continue;
        }
        const bool ok = std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == ' ';
        out += ok ? static_cast<char>(c) : '_';
        ++i;
    }

    // Dots and spaces at either end: ".." names the parent folder, a leading
    // dot hides the file, and Windows drops trailing ones on its own.
    const auto first = out.find_first_not_of(". ");
    if (first == std::string::npos) return "audio";
    out = out.substr(first, out.find_last_not_of(". ") - first + 1);

    // CON, NUL, COM1 and the rest are devices on Windows, extension or not.
    std::string stem = out.substr(0, out.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    static const char* kDevices[] = {"CON", "PRN", "AUX", "NUL"};
    const bool numbered = stem.size() == 4 && (stem.rfind("COM", 0) == 0 ||
                                               stem.rfind("LPT", 0) == 0) &&
                          stem[3] >= '1' && stem[3] <= '9';
    if (numbered || std::find(std::begin(kDevices), std::end(kDevices), stem) !=
                        std::end(kDevices)) {
        out = "_" + out;
    }

    // Most file systems stop at 255 bytes, and the upload's temp file puts a
    // prefix in front of this. Keep the extension; shorten the rest.
    constexpr std::size_t kMaxBytes = 150;
    if (out.size() > kMaxBytes) {
        const auto dot = out.rfind('.');
        const std::string ext =
            (dot != std::string::npos && out.size() - dot <= 16) ? out.substr(dot) : "";
        out = utf8::head(out.substr(0, out.size() - ext.size()), kMaxBytes - ext.size()) + ext;
    }
    return out;
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
