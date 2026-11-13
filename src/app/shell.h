// Handing a URL to the operating system.
//
// The window is not here any more: the desktop app (desktop/) is an Electron
// process that starts this one and talks to it over loopback. What is left is
// the one thing the engine still asks the OS to show -- a link from a summary,
// or the releases page -- which goes to the default browser.
#pragma once

#include <string>

namespace transcriptor::app {

// Best-effort: hand the URL to the OS default browser.
bool open_in_browser(const std::string& url);

}  // namespace transcriptor::app
