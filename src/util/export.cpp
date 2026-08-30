#include "util/export.h"
#include "util/lang.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <stdexcept>

#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <unistd.h>
#  include <sys/wait.h>
#endif

namespace transcriptor::exporter {

namespace {

void put_u32(std::string* s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_u16(std::string* s, std::uint16_t v) {
    for (int i = 0; i < 2; ++i) s->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

}  // namespace

fs::path new_session_dir(const std::string& base) {
    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm_buf);

    fs::path dir = paths::expand_user(base) / stamp;

    // Two recordings inside the same second must not collide.
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        for (int i = 2; i < 100; ++i) {
            fs::path alt = paths::expand_user(base) / (std::string(stamp) + "_" +
                                                       std::to_string(i));
            if (!fs::exists(alt, ec)) { dir = alt; break; }
        }
    }
    fs::create_directories(dir, ec);
    if (ec) {
        throw std::runtime_error(L("The output folder could not be created: ",
                                   "Kayıt klasörü oluşturulamadı: ") +
                                 paths::to_utf8(dir) + " (" + ec.message() + ")");
    }
    return dir;
}

bool save_audio_wav(const fs::path& path, const std::vector<float>& audio,
                    int samplerate) {
    const std::uint32_t frames = static_cast<std::uint32_t>(audio.size());
    const std::uint32_t data_bytes = frames * 2;

    std::string hdr;
    hdr.reserve(44);
    hdr += "RIFF";
    put_u32(&hdr, 36 + data_bytes);
    hdr += "WAVE";
    hdr += "fmt ";
    put_u32(&hdr, 16);                                     // PCM chunk size
    put_u16(&hdr, 1);                                      // format: PCM
    put_u16(&hdr, 1);                                      // channels: mono
    put_u32(&hdr, static_cast<std::uint32_t>(samplerate));
    put_u32(&hdr, static_cast<std::uint32_t>(samplerate) * 2);  // byte rate
    put_u16(&hdr, 2);                                      // block align
    put_u16(&hdr, 16);                                     // bits per sample
    hdr += "data";
    put_u32(&hdr, data_bytes);

    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));

    std::vector<std::int16_t> pcm(audio.size());
    for (std::size_t i = 0; i < audio.size(); ++i) {
        float v = std::clamp(audio[i], -1.0f, 1.0f);
        pcm[i] = static_cast<std::int16_t>(v * 32767.0f);
    }
    out.write(reinterpret_cast<const char*>(pcm.data()),
              static_cast<std::streamsize>(pcm.size() * sizeof(std::int16_t)));
    return out.good();
}

bool save_text(const fs::path& path, const std::string& text) {
    return paths::write_file(path, text);
}

bool save_json(const fs::path& path, const nlohmann::json& obj) {
    return paths::write_file(path, obj.dump(2));
}

bool open_in_file_manager(const fs::path& path) {
#ifdef _WIN32
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                                 SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(rc) > 32;
#else
#  ifdef __APPLE__
    const char* opener = "open";
#  else
    const char* opener = "xdg-open";
#  endif
    const std::string p = paths::to_utf8(path);
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // Detach from the app so the file manager outlives us.
        setsid();
        execlp(opener, opener, p.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    // Don't block on the file manager; just reap so it can't become a zombie.
    int status = 0;
    waitpid(pid, &status, WNOHANG);
    return true;
#endif
}

bool remove_session_dir(const fs::path& dir, const std::string& base) {
    std::error_code ec;
    if (dir.empty() || !fs::exists(dir, ec)) return false;

    const fs::path root = fs::weakly_canonical(paths::expand_user(base), ec);
    const fs::path target = fs::weakly_canonical(dir, ec);
    if (ec) return false;

    // Only delete inside the configured output directory.
    bool inside = (root == target);
    if (!inside) {
        for (fs::path p = target.parent_path(); !p.empty() && p != p.root_path();
             p = p.parent_path()) {
            if (p == root) { inside = true; break; }
        }
    }
    if (!inside) return false;

    fs::remove_all(target, ec);
    return !ec;
}

}  // namespace transcriptor::exporter
