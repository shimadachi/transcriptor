// Regression tests for the take that lived in RAM (R14).
//
// The bug: a recording was accumulated into one std::vector<float> that grew
// for the whole take. At 16 kHz that is 230 MB an hour, and a vector cannot
// grow in place -- each time it outgrew its capacity it allocated a larger
// block and copied the take into it, so a five-hour recording asked the host
// for two to three gigabytes at once, several times over. The app did not
// crash; the machine stopped responding. A user lost a five-hour meeting that
// way.
//
// The fix streams every block to a 16-bit WAV as it arrives, so the recorder's
// footprint is flat whatever the length of the take -- and the file is the
// session's audio.wav, already written when Stop is pressed.
//
// The headline checks here are the two memory ones: one proves the spooled
// path stays flat, and the other proves the probe that says so can actually
// see a buffer growing. Without the second, the first could pass for free.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/recorder.h"
#include "audio/spool.h"
#include "check.h"
#include "fake_capture.h"

#if defined(__linux__)
#  include <unistd.h>
#endif

using transcriptor::audio::AudioSource;
using transcriptor::audio::Recorder;
using transcriptor::audio::WavSpool;
using transcriptor::audio::max_spool_frames;
using transcriptor::audio::read_wav_f32;
using transcriptor::audio::recover_wav;
namespace fs = transcriptor::paths::fs;

namespace {

constexpr int kRate = 16000;

const AudioSource kSpeakers{"loop:speakers", "Speakers", true, 2, true};
const AudioSource kHeadset{"mic:headset", "Headset", false, 1, false};

fs::path g_scratch;

fs::path scratch(const std::string& name) { return g_scratch / name; }

std::vector<unsigned char> read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

std::uint32_t u32_at(const std::vector<unsigned char>& b, std::size_t at) {
    if (at + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(b[at]) |
           (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}
std::uint16_t u16_at(const std::vector<unsigned char>& b, std::size_t at) {
    if (at + 2 > b.size()) return 0;
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[at]) |
                                      (static_cast<std::uint16_t>(b[at + 1]) << 8));
}

bool tag_at(const std::vector<unsigned char>& b, std::size_t at, const char* tag) {
    return b.size() >= at + 4 && std::memcmp(b.data() + at, tag, 4) == 0;
}

// Resident set size in bytes, or 0 where it cannot be read. Only Linux is
// measured; the mechanism being tested is allocator-independent, and one
// platform proving it is enough to catch a regression.
std::size_t rss_bytes() {
#if defined(__linux__)
    std::ifstream in("/proc/self/statm");
    std::size_t total = 0, resident = 0;
    if (in >> total >> resident) {
        return resident * static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    }
#endif
    return 0;
}

// The drain thread polls every 20 ms; give it a moment to take the block.
bool wait_for_level(const Recorder& rec) {
    for (int i = 0; i < 200 && rec.level() <= 0.0f; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return rec.level() > 0.0f;
}

// ---------------------------------------------------------------------------
// The file the spool writes
// ---------------------------------------------------------------------------

void a_spooled_take_round_trips() {
    const fs::path path = scratch("round_trip.wav");
    std::vector<float> wrote(kRate);        // one second of a slow sine
    for (std::size_t i = 0; i < wrote.size(); ++i) {
        wrote[i] = 0.5f * std::sin(static_cast<float>(i) * 0.01f);
    }

    WavSpool spool;
    test::check("R14 the spool opens", spool.open(path, kRate), spool.error());
    test::check("R14 a block is written", spool.write(wrote));
    test::check("R14 the spool finishes", spool.finish(), spool.error());
    test::check("R14 the spool counted every frame",
                spool.frames() == wrote.size(),
                std::to_string(spool.frames()) + " frames");

    std::vector<float> back;
    std::string err;
    test::check("R14 the take reads back", read_wav_f32(path, &back, &err), err);
    test::check("R14 the take reads back whole", back.size() == wrote.size(),
                std::to_string(back.size()) + " of " + std::to_string(wrote.size()));

    float worst = 0.0f;
    for (std::size_t i = 0; i < std::min(back.size(), wrote.size()); ++i) {
        worst = std::max(worst, std::fabs(back[i] - wrote[i]));
    }
    // 16-bit quantization and nothing else.
    test::check("R14 the audio survives the round trip", worst < 2e-4f,
                "worst sample differs by " + std::to_string(worst));
}

void the_file_is_a_wav_any_player_can_open() {
    const fs::path path = scratch("header.wav");
    const std::size_t frames = 2 * kRate;
    std::vector<float> block(frames, 0.25f);

    WavSpool spool;
    spool.open(path, kRate);
    spool.write(block);
    spool.finish();

    const std::vector<unsigned char> b = read_bytes(path);
    test::check("R14 the file is header plus samples",
                b.size() == 44 + frames * 2,
                std::to_string(b.size()) + " bytes");
    test::check("R14 RIFF/WAVE/fmt /data are where a reader looks",
                tag_at(b, 0, "RIFF") && tag_at(b, 8, "WAVE") &&
                    tag_at(b, 12, "fmt ") && tag_at(b, 36, "data"));
    test::check("R14 the RIFF length field is patched at the end",
                u32_at(b, 4) == 36 + frames * 2,
                std::to_string(u32_at(b, 4)));
    test::check("R14 the data length field is patched at the end",
                u32_at(b, 40) == frames * 2, std::to_string(u32_at(b, 40)));
    test::check("R14 it is 16-bit mono PCM at the take's rate",
                u16_at(b, 20) == 1 && u16_at(b, 22) == 1 &&
                    u32_at(b, 24) == static_cast<std::uint32_t>(kRate) &&
                    u16_at(b, 32) == 2 && u16_at(b, 34) == 16);
    test::check("R14 the byte rate matches the sample rate",
                u32_at(b, 28) == static_cast<std::uint32_t>(kRate) * 2);
}

void samples_are_little_endian_whatever_the_host_is() {
    const fs::path path = scratch("endian.wav");
    WavSpool spool;
    spool.open(path, kRate);
    // 0.5 -> 16383 (0x3FFF) -> 0xFF 0x3F on disk, low byte first.
    const std::vector<float> one{0.5f};
    spool.write(one);
    spool.finish();

    const std::vector<unsigned char> b = read_bytes(path);
    test::check("R14 a sample is written low byte first",
                b.size() == 46 && b[44] == 0xFF && b[45] == 0x3F,
                b.size() == 46 ? std::to_string(b[44]) + " " + std::to_string(b[45])
                               : std::to_string(b.size()) + " bytes");
}

void samples_past_full_scale_are_clamped_not_wrapped() {
    const fs::path path = scratch("clamp.wav");
    // Gain and the limiter normally keep this in range; a bug upstream must
    // still not turn a loud passage into the opposite sign.
    const std::vector<float> loud{2.0f, -2.0f, 1.0f, -1.0f};
    WavSpool spool;
    spool.open(path, kRate);
    spool.write(loud);
    spool.finish();

    std::vector<float> back;
    std::string err;
    read_wav_f32(path, &back, &err);
    const bool clamped = back.size() == 4 && back[0] > 0.99f && back[1] < -0.99f &&
                         back[2] > 0.99f && back[3] < -0.99f;
    test::check("R14 samples past full scale clamp instead of wrapping", clamped);
}

// ---------------------------------------------------------------------------
// The memory the take does not use -- the bug itself
// ---------------------------------------------------------------------------

// 128 MB of float audio: 8.5 minutes at 16 kHz, enough that a buffer holding
// it is unmistakable in RSS and little enough to run in about a second.
constexpr std::size_t kBlockSamples = 4000;            // ~a quarter second
constexpr std::size_t kBlocks       = 8192;            // 32.7 M samples
constexpr std::size_t kAudioBytes   = kBlockSamples * kBlocks * sizeof(float);

void memory_stays_flat_while_spooling() {
    if (rss_bytes() == 0) {
        test::check("R14 (skipped: no RSS on this platform)", true);
        return;
    }
    const fs::path path = scratch("flat.wav");
    const std::vector<float> block(kBlockSamples, 0.1f);

    WavSpool spool;
    spool.open(path, kRate);
    const std::size_t before = rss_bytes();
    std::size_t peak = before;
    for (std::size_t i = 0; i < kBlocks; ++i) {
        spool.write(block);
        if ((i & 0xFF) == 0) peak = std::max(peak, rss_bytes());
    }
    peak = std::max(peak, rss_bytes());
    spool.finish();

    const std::size_t grew = peak > before ? peak - before : 0;
    // The spool stages 8 KB at a time and keeps nothing else, so anything in
    // the tens of megabytes means a take is being accumulated somewhere again.
    test::check("R14 spooling 128 MB of audio does not grow memory by 16 MB",
                grew < 16u * 1024 * 1024,
                "RSS grew " + std::to_string(grew / (1024 * 1024)) + " MB over " +
                    std::to_string(kAudioBytes / (1024 * 1024)) + " MB of audio");

    std::error_code ec;
    const std::uintmax_t on_disk = fs::file_size(path, ec);
    test::check("R14 all of it reached the disk",
                !ec && on_disk == 44 + static_cast<std::uintmax_t>(kBlockSamples) *
                                          kBlocks * 2,
                std::to_string(on_disk) + " bytes");
    fs::remove(path, ec);
}

void the_memory_probe_can_see_a_buffer_growing() {
    if (rss_bytes() == 0) {
        test::check("R14 (skipped: no RSS on this platform)", true);
        return;
    }
    // The old behaviour, on purpose: the same audio into one growing vector.
    // If this does not register, the check above proves nothing.
    const std::vector<float> block(kBlockSamples, 0.1f);
    std::vector<float> held;
    const std::size_t before = rss_bytes();
    for (std::size_t i = 0; i < kBlocks; ++i) {
        held.insert(held.end(), block.begin(), block.end());
    }
    const std::size_t grew = rss_bytes() - before;
    test::check("R14 the probe sees the old in-memory take grow",
                grew > 64u * 1024 * 1024,
                "RSS grew " + std::to_string(grew / (1024 * 1024)) + " MB holding " +
                    std::to_string(kAudioBytes / (1024 * 1024)) + " MB of audio");
}

// ---------------------------------------------------------------------------
// What is left on disk when the app does not get to finish
// ---------------------------------------------------------------------------

// Put a spool back into the state a crash leaves it in: blocks on disk, length
// fields still zero because finish() never ran.
void blank_the_length_fields(const fs::path& path) {
    std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
    const char zero[4] = {0, 0, 0, 0};
    io.seekp(4, std::ios::beg);
    io.write(zero, 4);
    io.seekp(40, std::ios::beg);
    io.write(zero, 4);
    io.close();
}

void a_take_survives_the_app_being_killed() {
    const fs::path path = scratch("killed.wav");
    const std::size_t frames = 3 * kRate;
    WavSpool spool;
    spool.open(path, kRate);
    spool.write(std::vector<float>(frames, 0.3f));
    spool.finish();
    blank_the_length_fields(path);

    // Read back without repairing first: the audio is still there, and the
    // file size is what says so. A take must never be lost to a header.
    std::vector<float> back;
    std::string err;
    test::check("R14 an unfinished spool still reads",
                read_wav_f32(path, &back, &err), err);
    test::check("R14 an unfinished spool reads back whole",
                back.size() == frames,
                std::to_string(back.size()) + " of " + std::to_string(frames));

    // And repairing it makes the file right for every other player too.
    test::check("R14 an unfinished spool repairs", recover_wav(path, &err), err);
    const std::vector<unsigned char> b = read_bytes(path);
    test::check("R14 repair restores both length fields",
                u32_at(b, 40) == frames * 2 && u32_at(b, 4) == 36 + frames * 2,
                "data=" + std::to_string(u32_at(b, 40)) +
                    " riff=" + std::to_string(u32_at(b, 4)));
    test::check("R14 repairing an intact file is a no-op that still succeeds",
                recover_wav(path, &err), err);
}

void a_spool_that_is_never_finished_is_still_closed() {
    const fs::path path = scratch("unfinished.wav");
    const std::size_t frames = kRate;
    {
        WavSpool spool;
        spool.open(path, kRate);
        spool.write(std::vector<float>(frames, 0.2f));
        // No finish(): the destructor has to patch the header, or an exception
        // on the way out of a take would leave a file claiming to be empty.
    }
    const std::vector<unsigned char> b = read_bytes(path);
    test::check("R14 a destroyed spool patches its own header",
                u32_at(b, 40) == frames * 2, std::to_string(u32_at(b, 40)));
}

void a_discarded_take_leaves_nothing_behind() {
    const fs::path path = scratch("discarded.wav");
    WavSpool spool;
    spool.open(path, kRate);
    spool.write(std::vector<float>(kRate, 0.4f));
    spool.discard();
    test::check("R14 discard removes the file", !fs::exists(path));
}

void the_format_runs_out_before_the_counter_does() {
    // WAV keeps its sizes in 32-bit fields. The spool must stop there rather
    // than wrap the header round and call a corrupt file a recording.
    const std::uint64_t frames = max_spool_frames();
    test::check("R14 the spool's ceiling is the WAV format's",
                frames * 2 <= 0xFFFFFFFFull - 36 &&
                    (frames + 1) * 2 > 0xFFFFFFFFull - 36,
                std::to_string(frames) + " frames");
    test::check("R14 that ceiling is over a day of recording",
                frames / kRate / 3600 >= 24,
                std::to_string(frames / kRate / 3600) + " hours");
}

// ---------------------------------------------------------------------------
// The recorder driving it
// ---------------------------------------------------------------------------

void a_recording_goes_to_disk_instead_of_memory() {
    fake_capture::reset();
    fake_capture::set_total_samples(2 * kRate);
    const fs::path path = scratch("take.wav");

    Recorder rec(kSpeakers, kRate);
    rec.spool_to(path);
    rec.start();
    test::check("R14 the meter still moves while spooling", wait_for_level(rec));
    const std::vector<float> returned = rec.stop();

    test::check("R14 a spooled take is not handed back in memory",
                returned.empty(),
                std::to_string(returned.size()) + " samples returned");
    test::check("R14 the recorder reports where the take is",
                rec.spooling() && rec.spool_path() == path);
    test::check("R14 nothing went wrong writing it", rec.spool_error().empty(),
                rec.spool_error());
    test::check("R14 the recorder counted the take",
                rec.captured() == static_cast<std::uint64_t>(2 * kRate),
                std::to_string(rec.captured()) + " samples");

    std::vector<float> back;
    std::string err;
    read_wav_f32(path, &back, &err);
    test::check("R14 the whole take is in the file",
                back.size() == static_cast<std::size_t>(2 * kRate),
                std::to_string(back.size()) + " samples");
}

void the_live_preview_still_gets_its_offsets() {
    // The tap's offset used to be the session buffer's size. Spooled, that
    // buffer stays empty -- so every block would have arrived at offset 0 and
    // the live transcript would have stacked the whole take on one instant.
    fake_capture::reset();
    // Delivered in real time, so the drain thread takes it in many blocks --
    // which is the only way this check means anything. A take handed over in
    // one block has nothing to get wrong after the first offset.
    fake_capture::set_source_stream(kSpeakers.id, 0.0, 0.3, 0.8f);
    const fs::path path = scratch("tapped.wav");

    std::vector<std::size_t> offsets;
    std::vector<std::size_t> sizes;
    std::mutex seen;

    Recorder rec(kSpeakers, kRate);
    rec.spool_to(path);
    rec.set_tap([&](const std::vector<float>& block, std::size_t offset) {
        std::lock_guard<std::mutex> lock(seen);
        offsets.push_back(offset);
        sizes.push_back(block.size());
    });
    rec.start();
    wait_for_level(rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    rec.stop();

    std::lock_guard<std::mutex> lock(seen);
    bool consecutive = !offsets.empty() && offsets.front() == 0;
    std::size_t at = 0;
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        if (offsets[i] != at) consecutive = false;
        at += sizes[i];
    }
    test::check("R14 a spooled take arrives at the tap in many blocks",
                offsets.size() > 4,
                std::to_string(offsets.size()) + " blocks");
    test::check("R14 a spooled take's tap offsets still run end to end",
                consecutive,
                std::to_string(offsets.size()) + " blocks covering " +
                    std::to_string(at) + " samples");

    // The three have to agree, or the live preview is describing a different
    // take from the one on disk.
    std::error_code ec;
    const std::uintmax_t on_disk = fs::file_size(path, ec);
    test::check("R14 the tap, the counter and the file agree on the take",
                !ec && rec.captured() == at &&
                    on_disk == 44 + static_cast<std::uintmax_t>(at) * 2,
                "tap " + std::to_string(at) + ", counted " +
                    std::to_string(rec.captured()) + ", file " +
                    std::to_string(on_disk > 44 ? (on_disk - 44) / 2 : 0));
}

void a_take_with_nowhere_to_write_is_still_recorded() {
    fake_capture::reset();
    fake_capture::set_total_samples(kRate);
    // A parent that is a file, not a folder. open() makes missing directories,
    // so an absent path is not enough to fail it -- but no platform will put a
    // directory inside a regular file, and running as root cannot help either.
    const fs::path blocker = scratch("not-a-folder");
    { std::ofstream make(blocker); make << "x"; }
    const fs::path path = blocker / "take.wav";

    Recorder rec(kSpeakers, kRate);
    rec.spool_to(path);
    rec.start();
    wait_for_level(rec);
    const std::vector<float> returned = rec.stop();

    test::check("R14 a take whose spool fails is kept in memory instead",
                returned.size() == static_cast<std::size_t>(kRate),
                std::to_string(returned.size()) + " samples");
    test::check("R14 and the app is told why", !rec.spool_error().empty(),
                rec.spool_error());
    test::check("R14 and does not believe there is a file",
                !rec.spooling() && rec.spool_path().empty());
}

void an_unspooled_take_is_unchanged() {
    fake_capture::reset();
    fake_capture::set_total_samples(2 * kRate);

    Recorder rec(kSpeakers, kRate);
    rec.start();
    wait_for_level(rec);
    const std::vector<float> take = rec.stop();
    test::check("R14 a recorder with no spool still returns the take",
                take.size() == static_cast<std::size_t>(2 * kRate),
                std::to_string(take.size()) + " samples");
}

void a_cancelled_take_is_removed_from_disk() {
    fake_capture::reset();
    fake_capture::set_total_samples(kRate);
    const fs::path path = scratch("cancelled.wav");

    Recorder rec(kSpeakers, kRate);
    rec.spool_to(path);
    rec.start();
    wait_for_level(rec);
    rec.stop();
    test::check("R14 the cancelled take was on disk a moment ago",
                fs::exists(path));
    rec.discard();
    test::check("R14 Cancel takes the spool with it", !fs::exists(path));
}

void a_paused_stretch_is_not_written() {
    fake_capture::reset();
    const fs::path path = scratch("paused.wav");

    Recorder rec(kSpeakers, kRate);
    rec.spool_to(path);
    rec.start();
    rec.pause();
    // Whatever the device delivers now is dropped, as it is in memory.
    fake_capture::set_total_samples(2 * kRate);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    rec.stop();

    std::error_code ec;
    const std::uintmax_t on_disk = fs::file_size(path, ec);
    test::check("R14 audio from a paused stretch is not spooled",
                !ec && on_disk == 44, std::to_string(on_disk) + " bytes");
}

void a_mixed_take_spools_the_mix() {
    fake_capture::reset();
    fake_capture::set_total_samples(2 * kRate);
    const fs::path path = scratch("mixed.wav");

    Recorder rec(kSpeakers, kRate, kHeadset);
    rec.spool_to(path);
    rec.start();
    wait_for_level(rec);
    rec.stop();

    std::vector<float> back;
    std::string err;
    read_wav_f32(path, &back, &err);
    test::check("R14 a mixed take spools the mix, not one source",
                back.size() == static_cast<std::size_t>(2 * kRate),
                std::to_string(back.size()) + " samples");
    // fake_capture hands both sources the same signal, so the sum is louder
    // than either -- which is how we know the file holds the mix.
    const bool summed = !back.empty() && std::fabs(back[kRate]) > 0.0f;
    test::check("R14 and the mix is audible in it", summed);
}

}  // namespace

int main(int argc, char** argv) {
    g_scratch = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "spool_scratch";
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    if (ec) {
        std::printf("FAIL  could not make the scratch directory\n");
        return 1;
    }

    a_spooled_take_round_trips();
    the_file_is_a_wav_any_player_can_open();
    samples_are_little_endian_whatever_the_host_is();
    samples_past_full_scale_are_clamped_not_wrapped();

    memory_stays_flat_while_spooling();
    the_memory_probe_can_see_a_buffer_growing();

    a_take_survives_the_app_being_killed();
    a_spool_that_is_never_finished_is_still_closed();
    a_discarded_take_leaves_nothing_behind();
    the_format_runs_out_before_the_counter_does();

    a_recording_goes_to_disk_instead_of_memory();
    the_live_preview_still_gets_its_offsets();
    a_take_with_nowhere_to_write_is_still_recorded();
    an_unspooled_take_is_unchanged();
    a_cancelled_take_is_removed_from_disk();
    a_paused_stretch_is_not_written();
    a_mixed_take_spools_the_mix();

    fs::remove_all(g_scratch, ec);
    return test::summary("spool");
}
