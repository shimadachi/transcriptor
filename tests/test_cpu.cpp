// Regression test for counting cores on a hybrid CPU (V39).
//
// util::physical_cores() sizes the thread pools of the diarizer, whisper and
// the summarizer. On Apple Silicon it already counted the performance cluster
// alone, because the efficiency cores are much slower and a compute-bound pool
// split evenly across both waits on them. On a hybrid Intel part under Linux it
// counted every core, efficiency ones included -- the same mistake the Apple
// branch exists to avoid.
//
// The query reads sysfs, so this hands it a tree laid out the way the kernel
// lays out a hybrid machine, and a plain one.

#include "util/cpu.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "check.h"

namespace fs = std::filesystem;

namespace {

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text << "\n";
}

// `cpus` logical CPUs, two SMT siblings per core for the first `smt_cores`
// cores and one thread each after that, as a hybrid part pairs its P-cores.
void machine(const fs::path& sys, int cpus, int smt_cores) {
    int core = 0;
    for (int cpu = 0; cpu < cpus;) {
        const int threads = core < smt_cores ? 2 : 1;
        for (int t = 0; t < threads && cpu < cpus; ++t, ++cpu) {
            const fs::path topo = sys / "devices" / "system" / "cpu" /
                                  ("cpu" + std::to_string(cpu)) / "topology";
            write(topo / "physical_package_id", "0");
            write(topo / "core_id", std::to_string(core));
        }
        ++core;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path scratch = argc > 1 ? argv[1] : "cpu_scratch";
    std::error_code ec;
    fs::remove_all(scratch, ec);

    // 6 performance cores with SMT (CPUs 0-11) and 8 efficiency cores (12-19):
    // the shape of a common hybrid laptop part.
    const fs::path hybrid = scratch / "hybrid";
    machine(hybrid, 20, 6);
    write(hybrid / "devices" / "cpu_core" / "cpus", "0-11");
    write(hybrid / "devices" / "cpu_atom" / "cpus", "12-19");
    const unsigned h = transcriptor::util::physical_cores_in(hybrid.string());
    test::check("V39 a hybrid part counts its performance cores alone", h == 6,
                std::to_string(h) + " cores");

    // The list the kernel writes can be broken up rather than one range.
    const fs::path split = scratch / "split";
    machine(split, 20, 6);
    write(split / "devices" / "cpu_core" / "cpus", "0-3,4,5-11");
    test::check("V39 a performance list in several pieces reads the same",
                transcriptor::util::physical_cores_in(split.string()) == 6);

    // One kind of core: no cpu_core list, and every core counts.
    const fs::path plain = scratch / "plain";
    machine(plain, 16, 8);
    const unsigned p = transcriptor::util::physical_cores_in(plain.string());
    test::check("a part with one kind of core still counts them all", p == 8,
                std::to_string(p) + " cores");

    test::check("no topology at all is reported as nothing usable",
                transcriptor::util::physical_cores_in((scratch / "empty").string()) == 0);

    return test::summary("cpu");
}
