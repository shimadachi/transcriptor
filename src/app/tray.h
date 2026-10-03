// The notification-area icon. Minimizing the window hides it there, and the
// icon's menu starts, stops and pauses a take without bringing the window back.
//
// Windows draws it with Shell_NotifyIcon. Linux publishes a StatusNotifierItem
// over D-Bus -- what KDE, GNOME's AppIndicator extension, XFCE and the wlroots
// bars all read -- spoken through GIO directly rather than libappindicator, so
// the binary gains no library a machine might not have. macOS has no tray to
// minimize into (the Dock is where a minimized window goes), so it gets none.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace transcriptor::app {

// Settings::tray, which the user picks in Settings → General.
enum class TrayMode {
    kMinimize,   // icon shown; minimizing hides the window in the tray
    kIcon,       // icon shown; minimizing goes to the taskbar as usual
    kOff,        // no icon at all
};

// Anything unrecognised is the default, the same rule config.json follows.
TrayMode tray_mode(const std::string& setting);

// What the tray has to reflect. Asked for whenever it may have changed, which
// is also how a new choice in Settings reaches it without a restart.
struct TrayStatus {
    bool     recording  = false;   // a take is running, in the app or in the page
    bool     paused     = false;
    bool     can_record = true;    // Start/Stop would do something: no job running
    TrayMode mode       = TrayMode::kMinimize;
};

enum class TrayCommand { kShowHide, kRecord, kPause, kQuit };

struct TrayItem {
    TrayCommand command;
    std::string label;
    bool        enabled          = true;
    bool        separator_before = false;
};

// The menu for `status`, in the interface language. Kept platform-free so the
// two implementations cannot drift into offering different things.
std::vector<TrayItem> tray_menu(const TrayStatus& status, bool window_shown);

// The hover text: the app's name, and what it is doing when that is a take.
std::string tray_tooltip(const TrayStatus& status);

class Tray {
public:
    // False where this build has no tray to offer (macOS, no native window),
    // so the setting is not shown for nothing.
    static bool supported();

    using StatusFn  = std::function<TrayStatus()>;
    // kShowHide never reaches this; the tray deals with the window itself.
    using CommandFn = std::function<void(TrayCommand)>;

    // `window` is the native top-level window: an HWND, or a GtkWindow*.
    // Everything here runs on the thread that owns it, callbacks included.
    Tray(void* window, StatusFn status, CommandFn on_command);
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    // Bring the window back from the tray, or from behind other windows.
    void show_window();

    // Re-read the status now rather than at the next tick.
    void refresh();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace transcriptor::app
