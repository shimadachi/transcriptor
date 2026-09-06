// Regression tests for the child-process runner (report findings R11, R12).
//
// R12: the read loop ended when the child closed stdout, then waited on it with
// no deadline and with the cancellation handle already released -- so a child
// that closed its output and kept running was both untimeoutable and
// uncancellable. curl and ffmpeg both do exactly that.
//
// R11 is the same defect on the Windows path, which cannot run here; the
// continuous-output case below is its POSIX twin and guards the shared shape.

#include "util/net.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "check.h"

using namespace transcriptor;

namespace {

struct Timed {
    net::ProcResult result;
    double          seconds;
};

Timed run_timed(const std::vector<std::string>& argv, double timeout,
                net::Canceller* cancel = nullptr) {
    const auto t0 = std::chrono::steady_clock::now();
    net::ProcResult r = net::run(argv, timeout, cancel);
    const double s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return {std::move(r), s};
}

std::string secs(double s) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3fs", s);
    return buf;
}

}  // namespace

int main() {
    // A child that closes stdout and stderr and then keeps running. The pipe
    // goes to EOF immediately, which used to end the wait entirely.
    {
        const Timed t = run_timed({"sh", "-c", "exec 1>&- 2>&-; sleep 3"}, 0.5);
        test::check("a child that closes stdout is still timed out",
                    t.seconds < 1.5, secs(t.seconds) + " for a 0.5s deadline");
    }

    // A child that never stops talking must not outrun its deadline either --
    // the drain loop has to keep looking at the clock.
    {
        const Timed t =
            run_timed({"sh", "-c", "while :; do echo chatter; done"}, 0.5);
        test::check("continuous output does not defeat the deadline",
                    t.seconds < 1.5, secs(t.seconds) + ", captured " +
                                         std::to_string(t.result.output.size()) +
                                         " bytes");
    }

    // Cancellation has to reach a child whose pipe has already closed: that is
    // the model download, which runs with no deadline of its own.
    {
        net::Canceller cancel;
        std::thread asker([&cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            cancel.request();
        });
        const Timed t =
            run_timed({"sh", "-c", "exec 1>&- 2>&-; sleep 5"}, 0, &cancel);
        asker.join();
        test::check("cancel reaches a child that closed stdout",
                    t.seconds < 2.0 && t.result.cancelled,
                    secs(t.seconds) + ", cancelled=" +
                        (t.result.cancelled ? "true" : "false"));
    }

    // A cancel that lands after the child has already exited must be harmless.
    // Reaping used to happen before the handle was released, leaving a window
    // where the signal could reach a PID the OS had handed to someone else.
    {
        net::Canceller cancel;
        const Timed t = run_timed({"sh", "-c", "exit 0"}, 5.0, &cancel);
        cancel.request();   // too late; must not signal anything
        test::check("a cancel after a clean exit is harmless",
                    t.result.exit_code == 0 && !t.result.cancelled,
                    "exit=" + std::to_string(t.result.exit_code));
    }

    // The ordinary cases still have to behave.
    {
        const Timed t = run_timed({"sh", "-c", "echo hello; exit 3"}, 5.0);
        test::check("a normal child returns its output and exit code",
                    t.result.launched && t.result.exit_code == 3 &&
                        t.result.output.rfind("hello", 0) == 0,
                    "exit=" + std::to_string(t.result.exit_code) + " in " +
                        secs(t.seconds));
    }
    {
        const Timed t = run_timed({"definitely-not-a-real-command-xyz"}, 5.0);
        test::check("a missing command reports not-launched", !t.result.launched);
    }
    {
        net::Canceller cancel;
        cancel.request();
        const net::ProcResult r = net::run({"sh", "-c", "echo nope"}, 5.0, &cancel);
        test::check("a cancel raised before the start never runs the child",
                    r.cancelled && r.output.empty());
    }

    return test::summary("process runner");
}
