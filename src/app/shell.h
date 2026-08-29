// The desktop window.
//
// The UI is the same HTML the Flask build served; this just hosts it in a
// native window (WebView2 on Windows, WKWebView on macOS, WebKitGTK on Linux)
// instead of a browser tab. Falls back to the default browser when the build
// has no webview or the platform runtime is missing.
#pragma once

#include <string>

namespace transcriptor::app {

// Blocks until the window closes. Returns false if no window could be created
// (the caller then keeps the server alive for the browser fallback).
bool run_window(const std::string& url, const std::string& title);

// Best-effort: hand the URL to the OS default browser.
bool open_in_browser(const std::string& url);

// True when this build was compiled with a native window.
bool has_native_window();

}  // namespace transcriptor::app
