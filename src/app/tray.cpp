#include "app/tray.h"

#include "util/lang.h"

namespace transcriptor::app {

TrayMode tray_mode(const std::string& setting) {
    if (setting == "icon") return TrayMode::kIcon;
    if (setting == "off") return TrayMode::kOff;
    return TrayMode::kMinimize;
}

std::vector<TrayItem> tray_menu(const TrayStatus& s, bool window_shown) {
    std::vector<TrayItem> items;
    items.push_back({TrayCommand::kShowHide,
                     window_shown ? L("Hide to tray", "Tepsiye gizle")
                                  : L("Show Transcriptor", "Transcriptor'ı göster")});
    items.push_back({TrayCommand::kRecord,
                     s.recording ? L("Stop recording", "Kaydı durdur")
                                 : L("Start recording", "Kaydı başlat"),
                     s.can_record, /*separator_before=*/true});
    items.push_back({TrayCommand::kPause,
                     s.paused ? L("Resume", "Sürdür") : L("Pause", "Duraklat"),
                     s.recording});
    items.push_back({TrayCommand::kQuit, L("Quit", "Çık"), true,
                     /*separator_before=*/true});
    return items;
}

std::string tray_tooltip(const TrayStatus& s) {
    if (s.recording) {
        return s.paused ? L("Transcriptor · paused", "Transcriptor · duraklatıldı")
                        : L("Transcriptor · recording", "Transcriptor · kayıtta");
    }
    if (!s.can_record) return L("Transcriptor · working…", "Transcriptor · işleniyor…");
    return "Transcriptor";
}

}  // namespace transcriptor::app

// ===========================================================================
#if defined(_WIN32)
// ===========================================================================

#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>

namespace transcriptor::app {

namespace {

constexpr UINT     kIconId   = 1;
constexpr UINT     kCallback = WM_APP + 0x7472;   // WM_APP..0xBFFF is ours to use
constexpr UINT_PTR kTimerId  = 0x7472;
constexpr wchar_t  kOwnerClass[] = L"TranscriptorTray";
constexpr wchar_t  kProp[]       = L"TranscriptorTray";

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

}  // namespace

struct Tray::Impl {
    HWND    window = nullptr;   // the app's; cleared if it is destroyed first
    HWND    owner  = nullptr;   // hidden, never shown: owns the icon and its menu
    WNDPROC window_proc = nullptr;   // the webview's, which ours hands on to
    HICON   icon = nullptr;
    bool    owns_icon = false;
    bool    icon_shown = false;
    UINT    taskbar_created = 0;
    TrayMode mode = TrayMode::kMinimize;   // as last applied
    std::wstring tip;
    StatusFn  status;
    CommandFn command;

    ~Impl() {
        if (owner) {
            KillTimer(owner, kTimerId);
            remove_icon();
            DestroyWindow(owner);
        }
        unhook();
        if (icon && owns_icon) DestroyIcon(icon);
    }

    NOTIFYICONDATAW base() const {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = owner;
        nid.uID = kIconId;
        return nid;
    }

    TrayStatus now() const { return status ? status() : TrayStatus{}; }

    bool add_icon(const TrayStatus& s) {
        NOTIFYICONDATAW nid = base();
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = kCallback;
        nid.hIcon = icon;
        tip = widen(tray_tooltip(s));
        lstrcpynW(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip));
        // After an Explorer restart the shell may or may not still list the
        // old icon, and NIM_ADD fails on one it does. Start from nothing.
        Shell_NotifyIconW(NIM_DELETE, &nid);
        icon_shown = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        if (icon_shown) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
        }
        return icon_shown;
    }

    void remove_icon() {
        if (!icon_shown) return;
        NOTIFYICONDATAW nid = base();
        Shell_NotifyIconW(NIM_DELETE, &nid);
        icon_shown = false;
    }

    // Bring the icon in line with the setting and the status. On the timer, so
    // a choice made in Settings lands within a second -- and an icon that could
    // not be added at login is tried again until Explorer is there to take it.
    void sync() {
        const TrayStatus s = now();
        mode = s.mode;
        if (mode == TrayMode::kOff) {
            if (icon_shown) {
                remove_icon();
                // The icon was the way back to a hidden window, so it comes back.
                if (window && !IsWindowVisible(window)) show_window();
            }
            return;
        }
        if (!icon_shown) {
            add_icon(s);
            return;
        }
        const std::wstring next = widen(tray_tooltip(s));
        if (next == tip) return;
        tip = next;
        NOTIFYICONDATAW nid = base();
        nid.uFlags = NIF_TIP | NIF_SHOWTIP;
        lstrcpynW(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip));
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    bool shown() const { return window && IsWindowVisible(window); }

    void show_window() {
        if (!window) return;
        // A window minimized into the tray is hidden *and* still minimized, so
        // SW_SHOW alone would bring it back as a taskbar button and no more.
        ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window);
    }

    void hide_window() {
        if (window && icon_shown) ShowWindow(window, SW_HIDE);
    }

    void run(TrayCommand c) {
        if (c == TrayCommand::kShowHide) {
            shown() ? hide_window() : show_window();
        } else if (command) {
            command(c);
        }
    }

    void show_menu(POINT at) {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        for (const TrayItem& item : tray_menu(now(), shown())) {
            if (item.separator_before) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            // +1 because TrackPopupMenuEx reports "nothing chosen" as 0.
            AppendMenuW(menu, MF_STRING | (item.enabled ? MF_ENABLED : MF_GRAYED),
                        static_cast<UINT_PTR>(item.command) + 1,
                        widen(item.label).c_str());
        }
        // The documented dance for a tray menu: without the foreground it stays
        // open when the user clicks somewhere else, and without the WM_NULL
        // after it the next one can close the moment it opens.
        SetForegroundWindow(owner);
        UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON;
        flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
        const UINT chosen = static_cast<UINT>(
            TrackPopupMenuEx(menu, flags, at.x, at.y, owner, nullptr));
        PostMessageW(owner, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (chosen) run(static_cast<TrayCommand>(chosen - 1));
    }

    // Put the webview's own window procedure back. Only when ours is still the
    // one installed: restoring over somebody else's would cut them out.
    void unhook() {
        if (!window) return;
        if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) ==
            &window_hook) {
            SetWindowLongPtrW(window, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(window_proc));
        }
        RemovePropW(window, kProp);
        window = nullptr;
    }

    static LRESULT CALLBACK owner_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* t = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!t) return DefWindowProcW(hwnd, msg, wp, lp);

        if (msg == kCallback) {
            // NOTIFYICON_VERSION_4: the event in LOWORD(lp), the anchor in wp.
            switch (LOWORD(lp)) {
            case WM_CONTEXTMENU:
                t->show_menu({GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
                break;
            case NIN_SELECT:
            case NIN_KEYSELECT:
                t->show_window();
                break;
            }
            return 0;
        }
        // Explorer restarted, and took every notification icon with it.
        if (t->taskbar_created && msg == t->taskbar_created) {
            if (t->mode != TrayMode::kOff) t->add_icon(t->now());
            return 0;
        }
        if (msg == WM_TIMER && wp == kTimerId) {
            t->sync();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // Sits in front of the webview's window procedure. Minimized -- when that is
    // what the user chose -- the window is hidden as well, which takes its
    // taskbar button away: the icon is now the way back. WM_SIZE rather than SC_MINIMIZE, because Win+M, Win+Down and a
    // click on the active taskbar button minimize without the system menu.
    static LRESULT CALLBACK window_hook(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* t = static_cast<Impl*>(GetPropW(hwnd, kProp));
        if (!t) return DefWindowProcW(hwnd, msg, wp, lp);
        const WNDPROC next = t->window_proc;
        if (msg == WM_NCDESTROY) t->unhook();
        const LRESULT result = CallWindowProcW(next, hwnd, msg, wp, lp);
        if (msg == WM_SIZE && wp == SIZE_MINIMIZED && t->mode == TrayMode::kMinimize) {
            t->hide_window();
        }
        return result;
    }
};

Tray::Tray(void* window, StatusFn status, CommandFn on_command) : impl_(new Impl) {
    Impl& t = *impl_;
    t.status = std::move(status);
    t.command = std::move(on_command);

    const HINSTANCE self = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Impl::owner_proc;
    wc.hInstance = self;
    wc.lpszClassName = kOwnerClass;
    RegisterClassExW(&wc);   // fails harmlessly when a previous Tray registered it

    // Not a message-only window: those never see broadcasts, and TaskbarCreated
    // is one.
    t.owner = CreateWindowExW(WS_EX_TOOLWINDOW, kOwnerClass, L"", WS_POPUP,
                              0, 0, 0, 0, nullptr, nullptr, self, &t);
    if (!t.owner) return;   // no icon: minimizing then works as it always did

    t.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    // An elevated process would otherwise never hear it from a normal Explorer.
    if (t.taskbar_created) {
        ChangeWindowMessageFilterEx(t.owner, t.taskbar_created, MSGFLT_ALLOW, nullptr);
    }

    // The same resource as the window and the .exe (src/platform/app.rc), at the
    // size the notification area draws, so the tray shows the app's own icon.
    t.icon = static_cast<HICON>(LoadImageW(self, MAKEINTRESOURCEW(32512), IMAGE_ICON,
                                           GetSystemMetrics(SM_CXSMICON),
                                           GetSystemMetrics(SM_CYSMICON),
                                           LR_DEFAULTCOLOR));
    t.owns_icon = t.icon != nullptr;
    if (!t.icon) t.icon = LoadIconW(nullptr, IDI_APPLICATION);

    // Adds the icon unless the setting says not to. Failing here is not final:
    // Explorer may not be up yet at login, and the timer tries again.
    t.sync();
    SetTimer(t.owner, kTimerId, 1000, nullptr);

    // Both in place before the hook is, since the hook reads them.
    t.window = static_cast<HWND>(window);
    if (t.window) {
        SetPropW(t.window, kProp, &t);
        t.window_proc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(t.window, GWLP_WNDPROC));
        SetWindowLongPtrW(t.window, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(&Impl::window_hook));
    }
}

Tray::~Tray() = default;

bool Tray::supported() { return true; }
void Tray::show_window() { impl_->show_window(); }
void Tray::refresh() { impl_->sync(); }

}  // namespace transcriptor::app

// ===========================================================================
#elif defined(TRANSCRIPTOR_HAVE_WEBVIEW) && !defined(__APPLE__)
// ===========================================================================

#include <atomic>
#include <cstdio>
#include <cstring>

#include <gtk/gtk.h>
#include <unistd.h>

#include "tray_icon.h"   // generated from assets/logo-256.png by CMake

namespace transcriptor::app {

namespace {

constexpr const char* kItemIface    = "org.kde.StatusNotifierItem";
constexpr const char* kMenuIface    = "com.canonical.dbusmenu";
constexpr const char* kItemPath     = "/StatusNotifierItem";
constexpr const char* kMenuPath     = "/MenuBar";
constexpr const char* kWatcher      = "org.kde.StatusNotifierWatcher";
constexpr const char* kWatcherPath  = "/StatusNotifierWatcher";

// Both interfaces as the hosts expect them. GDBus refuses a property or a
// method that is not declared here, so this is the contract, not decoration.
constexpr const char kIntrospection[] = R"XML(<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="IconThemePath" type="s" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionMovieName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <method name="ContextMenu">
      <arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/>
    </method>
    <method name="Activate">
      <arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/>
    </method>
    <method name="SecondaryActivate">
      <arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/>
    </method>
    <method name="Scroll">
      <arg name="delta" type="i" direction="in"/>
      <arg name="orientation" type="s" direction="in"/>
    </method>
    <signal name="NewTitle"/>
    <signal name="NewIcon"/>
    <signal name="NewAttentionIcon"/>
    <signal name="NewOverlayIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg name="status" type="s"/></signal>
  </interface>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout">
      <arg name="parentId" type="i" direction="in"/>
      <arg name="recursionDepth" type="i" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="revision" type="u" direction="out"/>
      <arg name="layout" type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="properties" type="a(ia{sv})" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg name="id" type="i" direction="in"/>
      <arg name="name" type="s" direction="in"/>
      <arg name="value" type="v" direction="out"/>
    </method>
    <method name="Event">
      <arg name="id" type="i" direction="in"/>
      <arg name="eventId" type="s" direction="in"/>
      <arg name="data" type="v" direction="in"/>
      <arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg name="events" type="a(isvu)" direction="in"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <method name="AboutToShow">
      <arg name="id" type="i" direction="in"/>
      <arg name="needUpdate" type="b" direction="out"/>
    </method>
    <method name="AboutToShowGroup">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="updatesNeeded" type="ai" direction="out"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <signal name="ItemsPropertiesUpdated">
      <arg name="updatedProps" type="a(ia{sv})"/>
      <arg name="removedProps" type="a(ias)"/>
    </signal>
    <signal name="LayoutUpdated">
      <arg name="revision" type="u"/><arg name="parent" type="i"/>
    </signal>
    <signal name="ItemActivationRequested">
      <arg name="id" type="i"/><arg name="timestamp" type="u"/>
    </signal>
  </interface>
</node>)XML";

// dbusmenu ids. 0 is the root. An item's id follows from its command, never
// from its position, so a host clicking from a layout it cached a moment ago
// still reaches the item it showed.
int item_id(TrayCommand c) { return 1 + static_cast<int>(c); }
int separator_id(std::size_t index) { return 100 + static_cast<int>(index); }

// '_' marks a mnemonic in a dbusmenu label; a literal one is written twice.
std::string menu_label(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '_') out += '_';
        out += c;
    }
    return out;
}

GVariant* empty_pixmaps() {
    return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0);
}

// The embedded PNG, decoded by cairo's own reader. Not gdk-pixbuf's: newer ones
// hand every image to glycin, a sandboxed loader process, and where its sandbox
// cannot start -- a container, a locked-down user namespace -- the tray icon
// would come up blank. Cairo is linked by GTK anyway, and reads PNG itself.
cairo_surface_t* load_app_icon() {
    struct Reader {
        const unsigned char* at;
        std::size_t          left;
    } reader{kTrayIconPng, sizeof(kTrayIconPng)};
    cairo_surface_t* icon = cairo_image_surface_create_from_png_stream(
        [](void* closure, unsigned char* out, unsigned int length) {
            auto* r = static_cast<Reader*>(closure);
            if (length > r->left) return CAIRO_STATUS_READ_ERROR;
            std::memcpy(out, r->at, length);
            r->at += length;
            r->left -= length;
            return CAIRO_STATUS_SUCCESS;
        },
        &reader);
    if (cairo_surface_status(icon) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(icon);
        return nullptr;
    }
    return icon;
}

// The app's icon as StatusNotifierItem pixmaps: ARGB32 in network byte order,
// not premultiplied, at the sizes panels draw. Sent as pixels rather than as an
// icon name because a name only resolves once the .desktop file and its icon
// are installed, and the binary is just as often run out of the unpacked archive.
GVariant* icon_pixmaps(cairo_surface_t* src) {
    const int src_w = cairo_image_surface_get_width(src);
    const int src_h = cairo_image_surface_get_height(src);
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(iiay)"));
    for (const int size : {16, 22, 24, 32, 48, 64}) {
        cairo_surface_t* dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        cairo_t* cr = cairo_create(dst);
        cairo_scale(cr, double(size) / src_w, double(size) / src_h);
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_flush(dst);

        // Cairo's ARGB32 is a native-endian word per pixel, premultiplied.
        const unsigned char* data = cairo_image_surface_get_data(dst);
        const int stride = cairo_image_surface_get_stride(dst);
        std::vector<guchar> argb;
        argb.reserve(static_cast<std::size_t>(size) * size * 4);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                guint32 px = 0;
                std::memcpy(&px, data + y * stride + x * 4, sizeof px);
                const guint a = px >> 24;
                guint r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, bl = px & 0xff;
                if (a != 0 && a != 255) {
                    r = (r * 255 + a / 2) / a;
                    g = (g * 255 + a / 2) / a;
                    bl = (bl * 255 + a / 2) / a;
                }
                argb.push_back(static_cast<guchar>(a));
                argb.push_back(static_cast<guchar>(r));
                argb.push_back(static_cast<guchar>(g));
                argb.push_back(static_cast<guchar>(bl));
            }
        }
        cairo_surface_destroy(dst);
        g_variant_builder_add(&b, "(ii@ay)", size, size,
                              g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, argb.data(),
                                                        argb.size(), 1));
    }
    return g_variant_ref_sink(g_variant_builder_end(&b));
}

// What every GLib callback is handed instead of the Impl itself. The Impl
// clears `impl` when it goes; the link lives on until the last registration,
// subscription or call holding it lets go. GDBus may still deliver something
// queued after an unregister -- and webview drains the main loop while it
// tears the window down, after the tray is gone -- so a raw Impl* there would
// be a use-after-free in the middle of shutting down, ahead of the code that
// saves a take still in progress.
struct Link {
    Tray::Impl*      impl = nullptr;
    std::atomic<int> refs{1};
};

Link* link_ref(Link* l) {
    l->refs.fetch_add(1);
    return l;
}

void link_unref(gpointer p) {
    auto* l = static_cast<Link*>(p);
    if (l->refs.fetch_sub(1) == 1) delete l;
}

Tray::Impl* impl_of(gpointer p) { return static_cast<Link*>(p)->impl; }

}  // namespace

struct Tray::Impl {
    GtkWindow*       window = nullptr;   // nulled by GObject if it goes first
    StatusFn         status;
    CommandFn        command;
    Link*            link = nullptr;

    GDBusConnection* bus = nullptr;
    GCancellable*    cancel = nullptr;
    std::string      bus_name;
    guint            item_reg = 0, menu_reg = 0, owner_id = 0, watch_id = 0;
    guint            host_sub = 0, timer = 0;
    gulong           state_handler = 0;

    // The icon is on screen only when all of these hold. Until then, and
    // whenever it goes away, minimizing must leave the window where it can be
    // found again.
    bool named = false, watcher = false, registered = false, host = false;

    TrayMode mode = TrayMode::kMinimize;   // as last applied
    bool     ready = false;                // objects exported; the name may be taken

    std::vector<TrayItem> items;     // as last published
    std::string           tooltip;
    guint32               revision = 1;
    GVariant*             pixmaps = nullptr;

    bool have_pos = false;
    gint pos_x = 0, pos_y = 0;

    ~Impl() {
        link->impl = nullptr;
        if (timer) g_source_remove(timer);
        if (window) {
            if (state_handler) g_signal_handler_disconnect(window, state_handler);
            g_object_remove_weak_pointer(G_OBJECT(window),
                                         reinterpret_cast<gpointer*>(&window));
        }
        if (cancel) g_cancellable_cancel(cancel);
        if (bus) {
            if (host_sub) g_dbus_connection_signal_unsubscribe(bus, host_sub);
            if (watch_id) g_bus_unwatch_name(watch_id);
            if (owner_id) g_bus_unown_name(owner_id);
            if (item_reg) g_dbus_connection_unregister_object(bus, item_reg);
            if (menu_reg) g_dbus_connection_unregister_object(bus, menu_reg);
            g_object_unref(bus);
        }
        if (cancel) g_object_unref(cancel);
        if (pixmaps) g_variant_unref(pixmaps);
        link_unref(link);
    }

    bool visible() const { return named && watcher && registered && host; }
    bool shown() const { return window && gtk_widget_get_visible(GTK_WIDGET(window)); }

    void show_window() {
        if (!window) return;
        if (!shown()) {
            gtk_window_deiconify(window);
            if (have_pos) gtk_window_move(window, pos_x, pos_y);   // X11 only
            gtk_widget_show(GTK_WIDGET(window));
        }
        gtk_window_present(window);
        refresh();
    }

    void hide_window() {
        if (!window || !visible()) return;
        gtk_window_get_position(window, &pos_x, &pos_y);
        have_pos = true;
        gtk_widget_hide(GTK_WIDGET(window));
        refresh();
    }

    void run(TrayCommand c) {
        if (c == TrayCommand::kShowHide) {
            shown() ? hide_window() : show_window();
        } else if (command) {
            command(c);
        }
    }

    // Called whenever one of the conditions behind visible() changes.
    void icon_changed() {
        if (!visible() && !shown()) show_window();
    }

    void emit(const char* path, const char* iface, const char* signal,
              GVariant* args = nullptr) {
        if (bus) g_dbus_connection_emit_signal(bus, nullptr, path, iface, signal, args, nullptr);
    }

    // The bus name is what puts the item on a panel, and letting it go is how
    // an item leaves one: the watcher sees the owner vanish and drops it.
    void own_name() {
        owner_id = g_bus_own_name_on_connection(
            bus, bus_name.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE, &on_name_acquired,
            &on_name_lost, link_ref(link), link_unref);
    }

    void apply_mode(TrayMode m) {
        mode = m;
        if (m != TrayMode::kOff && !owner_id) {
            own_name();
        } else if (m == TrayMode::kOff && owner_id) {
            g_bus_unown_name(owner_id);
            owner_id = 0;
            named = registered = host = false;
            icon_changed();
        }
    }

    // Re-read the status and tell the host what changed. True when the menu did.
    // Also where a new choice in Settings takes effect.
    bool refresh() {
        const TrayStatus s = status ? status() : TrayStatus{};
        if (ready) apply_mode(s.mode);
        std::vector<TrayItem> next = tray_menu(s, shown());
        bool menu_changed = next.size() != items.size();
        for (std::size_t i = 0; !menu_changed && i < next.size(); ++i) {
            menu_changed = next[i].command != items[i].command ||
                           next[i].label != items[i].label ||
                           next[i].enabled != items[i].enabled ||
                           next[i].separator_before != items[i].separator_before;
        }
        if (menu_changed) {
            items = std::move(next);
            ++revision;
            emit(kMenuPath, kMenuIface, "LayoutUpdated",
                 g_variant_new("(ui)", revision, 0));
        }
        std::string tip = tray_tooltip(s);
        if (tip != tooltip) {
            tooltip = std::move(tip);
            emit(kItemPath, kItemIface, "NewToolTip");
        }
        return menu_changed;
    }

    // -- dbusmenu ---------------------------------------------------------

    static GVariant* props(const TrayItem* item) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        if (item) {
            g_variant_builder_add(&b, "{sv}", "label",
                                  g_variant_new_string(menu_label(item->label).c_str()));
            g_variant_builder_add(&b, "{sv}", "enabled",
                                  g_variant_new_boolean(item->enabled));
        } else {
            g_variant_builder_add(&b, "{sv}", "type", g_variant_new_string("separator"));
        }
        return g_variant_builder_end(&b);
    }

    static GVariant* root_props() {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "children-display",
                              g_variant_new_string("submenu"));
        return g_variant_builder_end(&b);
    }

    // Every entry under the root, separators included, in menu order.
    template <typename F>
    void for_each_entry(F&& f) const {
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].separator_before) f(separator_id(i), nullptr);
            f(item_id(items[i].command), &items[i]);
        }
    }

    bool has_entry(int id) const {
        bool found = id == 0;
        for_each_entry([&](int each, const TrayItem*) { found = found || each == id; });
        return found;
    }

    // Properties of `id`, floating, or nullptr when there is no such entry.
    GVariant* props_of(int id) const {
        if (id == 0) return root_props();
        GVariant* found = nullptr;
        for_each_entry([&](int each, const TrayItem* item) {
            if (each == id && !found) found = props(item);
        });
        return found;
    }

    static GVariant* node(int id, GVariant* properties, GVariant* children) {
        return g_variant_new("(i@a{sv}@av)", id, properties, children);
    }

    static GVariant* no_children() {
        return g_variant_new_array(G_VARIANT_TYPE_VARIANT, nullptr, 0);
    }

    void click(int id) {
        for (const TrayItem& item : items) {
            if (item_id(item.command) == id && item.enabled) {
                run(item.command);
                return;
            }
        }
    }

    void menu_call(const gchar* method, GVariant* params, GDBusMethodInvocation* inv) {
        const std::string m = method;
        if (m == "GetLayout") {
            gint32 parent = 0, depth = 0;
            g_variant_get(params, "(ii@as)", &parent, &depth, nullptr);
            GVariant* layout = nullptr;
            if (parent == 0) {
                GVariantBuilder kids;
                g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
                if (depth != 0) {
                    for_each_entry([&](int id, const TrayItem* item) {
                        g_variant_builder_add(&kids, "v",
                                              node(id, props(item), no_children()));
                    });
                }
                layout = node(0, root_props(), g_variant_builder_end(&kids));
            } else if (GVariant* p = props_of(parent)) {
                layout = node(parent, p, no_children());
            } else {
                g_dbus_method_invocation_return_dbus_error(
                    inv, "org.freedesktop.DBus.Error.InvalidArgs", "No such menu item");
                return;
            }
            g_dbus_method_invocation_return_value(
                inv, g_variant_new("(u@(ia{sv}av))", revision, layout));
        } else if (m == "GetGroupProperties") {
            GVariant* ids = g_variant_get_child_value(params, 0);
            GVariantBuilder out;
            g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
            if (g_variant_n_children(ids) == 0) {
                for_each_entry([&](int id, const TrayItem* item) {
                    g_variant_builder_add(&out, "(i@a{sv})", id, props(item));
                });
            } else {
                gsize n = 0;
                const auto* wanted = static_cast<const gint32*>(
                    g_variant_get_fixed_array(ids, &n, sizeof(gint32)));
                for (gsize i = 0; i < n; ++i) {
                    if (GVariant* p = props_of(wanted[i])) {
                        g_variant_builder_add(&out, "(i@a{sv})", wanted[i], p);
                    }
                }
            }
            g_variant_unref(ids);
            g_dbus_method_invocation_return_value(inv, g_variant_new("(@a(ia{sv}))",
                                                  g_variant_builder_end(&out)));
        } else if (m == "GetProperty") {
            gint32 id = 0;
            const gchar* name = nullptr;
            g_variant_get(params, "(i&s)", &id, &name);
            GVariant* value = nullptr;
            if (GVariant* p = props_of(id)) {
                g_variant_ref_sink(p);
                value = g_variant_lookup_value(p, name, nullptr);
                g_variant_unref(p);
            }
            if (!value) {
                g_dbus_method_invocation_return_dbus_error(
                    inv, "org.freedesktop.DBus.Error.InvalidArgs", "No such property");
                return;
            }
            g_dbus_method_invocation_return_value(inv, g_variant_new("(@v)",
                                                  g_variant_new_variant(value)));
            g_variant_unref(value);
        } else if (m == "Event") {
            gint32 id = 0;
            const gchar* event = nullptr;
            g_variant_get(params, "(i&s@vu)", &id, &event, nullptr, nullptr);
            const bool clicked = g_strcmp0(event, "clicked") == 0;
            g_dbus_method_invocation_return_value(inv, nullptr);
            if (clicked) click(id);   // after the reply: quitting may follow
        } else if (m == "EventGroup") {
            GVariantIter* it = nullptr;
            g_variant_get(params, "(a(isvu))", &it);
            std::vector<int> clicks;
            GVariantBuilder errors;
            g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
            gint32 id = 0;
            const gchar* event = nullptr;
            while (g_variant_iter_next(it, "(i&svu)", &id, &event, nullptr, nullptr)) {
                if (!has_entry(id)) {
                    g_variant_builder_add(&errors, "i", id);
                } else if (g_strcmp0(event, "clicked") == 0) {
                    clicks.push_back(id);
                }
            }
            g_variant_iter_free(it);
            g_dbus_method_invocation_return_value(inv, g_variant_new("(@ai)",
                                                  g_variant_builder_end(&errors)));
            for (int c : clicks) click(c);
        } else if (m == "AboutToShow") {
            // The host is about to draw the menu: make it the current one.
            g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", refresh()));
        } else if (m == "AboutToShowGroup") {
            GVariantBuilder updates;
            g_variant_builder_init(&updates, G_VARIANT_TYPE("ai"));
            if (refresh()) g_variant_builder_add(&updates, "i", 0);
            g_dbus_method_invocation_return_value(
                inv, g_variant_new("(@ai@ai)", g_variant_builder_end(&updates),
                                   g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
        } else {
            g_dbus_method_invocation_return_dbus_error(
                inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
        }
    }

    GVariant* menu_property(const gchar* name) const {
        const std::string p = name;
        if (p == "Version") return g_variant_new_uint32(3);
        if (p == "TextDirection") return g_variant_new_string("ltr");
        if (p == "Status") return g_variant_new_string("normal");
        if (p == "IconThemePath") return g_variant_new_strv(nullptr, 0);
        return nullptr;
    }

    // -- the item itself ----------------------------------------------------

    void item_call(const gchar* method, GDBusMethodInvocation* inv) {
        g_dbus_method_invocation_return_value(inv, nullptr);
        // A left click. ContextMenu goes unused while Menu is set, and there is
        // nothing sensible for a middle click or a scroll to do.
        if (g_strcmp0(method, "Activate") == 0) show_window();
    }

    GVariant* item_property(const gchar* name) const {
        const std::string p = name;
        if (p == "Category") return g_variant_new_string("ApplicationStatus");
        if (p == "Id") return g_variant_new_string("transcriptor");
        if (p == "Title") return g_variant_new_string("Transcriptor");
        if (p == "Status") return g_variant_new_string("Active");
        if (p == "WindowId") return g_variant_new_int32(0);
        if (p == "IconPixmap") return pixmaps ? g_variant_ref(pixmaps) : empty_pixmaps();
        if (p == "OverlayIconPixmap" || p == "AttentionIconPixmap") return empty_pixmaps();
        if (p == "IconName" || p == "IconThemePath" || p == "OverlayIconName" ||
            p == "AttentionIconName" || p == "AttentionMovieName") {
            return g_variant_new_string("");
        }
        if (p == "ToolTip") {
            return g_variant_new("(s@a(iiay)ss)", "", empty_pixmaps(),
                                 tooltip.c_str(), "");
        }
        if (p == "ItemIsMenu") return g_variant_new_boolean(FALSE);
        if (p == "Menu") return g_variant_new_object_path(kMenuPath);
        return nullptr;
    }

    // -- the watcher, which is what puts an item on a panel ------------------

    void try_register() {
        if (!named || !watcher) return;
        g_dbus_connection_call(
            bus, kWatcher, kWatcherPath, kWatcher, "RegisterStatusNotifierItem",
            g_variant_new("(s)", bus_name.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
            cancel, &on_registered, link_ref(link));
    }

    // A watcher can run with no panel drawing items for it, and an item it
    // accepted there is invisible. Ask, rather than take the registration as
    // proof that anybody can see the icon.
    void ask_for_host() {
        g_dbus_connection_call(
            bus, kWatcher, kWatcherPath, "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", kWatcher, "IsStatusNotifierHostRegistered"),
            G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, cancel, &on_host_answer,
            link_ref(link));
    }

    static void on_registered(GObject* source, GAsyncResult* res, gpointer data) {
        GError* err = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
        if (Impl* t = impl_of(data)) {
            // Not if the name was let go while the call was out: the panel has
            // already dropped the item it is answering about.
            t->registered = reply != nullptr && t->named;
            if (err) {
                std::fprintf(stderr, "tray: the panel refused the icon (%s)\n", err->message);
            }
            if (t->registered) t->ask_for_host();
            t->icon_changed();
        }
        if (reply) g_variant_unref(reply);
        g_clear_error(&err);
        link_unref(data);
    }

    static void on_host_answer(GObject* source, GAsyncResult* res, gpointer data) {
        GError* err = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
        if (Impl* t = impl_of(data)) {
            // A watcher too old to have the property says nothing either way;
            // the ones that have no host are the ones that do have it.
            bool host = true;
            if (reply) {
                GVariant* v = nullptr;
                g_variant_get(reply, "(v)", &v);
                if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
                    host = g_variant_get_boolean(v);
                }
                if (v) g_variant_unref(v);
            }
            t->host = host;
            t->icon_changed();
        }
        if (reply) g_variant_unref(reply);
        g_clear_error(&err);
        link_unref(data);
    }

    static void on_host_signal(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                               const gchar* signal, GVariant*, gpointer data) {
        Impl* t = impl_of(data);
        if (!t || !t->registered) return;
        if (g_strcmp0(signal, "StatusNotifierHostRegistered") == 0) {
            t->host = true;
            t->icon_changed();
        } else if (g_strcmp0(signal, "StatusNotifierHostUnregistered") == 0) {
            t->ask_for_host();   // another panel may still be drawing it
        }
    }

    static void on_name_acquired(GDBusConnection*, const gchar*, gpointer data) {
        if (Impl* t = impl_of(data)) {
            t->named = true;
            t->try_register();
        }
    }

    static void on_name_lost(GDBusConnection*, const gchar*, gpointer data) {
        if (Impl* t = impl_of(data)) {
            t->named = false;
            t->icon_changed();
        }
    }

    static void on_watcher_appeared(GDBusConnection*, const gchar*, const gchar*,
                                    gpointer data) {
        if (Impl* t = impl_of(data)) {
            t->watcher = true;
            t->try_register();
        }
    }

    // The panel went away -- it crashed, or is restarting. Its replacement
    // brings a new watcher, and the item registers again then.
    static void on_watcher_vanished(GDBusConnection*, const gchar*, gpointer data) {
        if (Impl* t = impl_of(data)) {
            t->watcher = false;
            t->registered = false;
            t->host = false;
            t->icon_changed();
        }
    }

    // GTK reports minimizing as a window state; under Wayland it never does,
    // because the protocol has no way to say so. There "Hide to tray" in the
    // icon's menu is the way in.
    static gboolean on_window_state(GtkWidget*, GdkEventWindowState* e, gpointer data) {
        Impl* t = impl_of(data);
        if (t && t->mode == TrayMode::kMinimize &&
            (e->changed_mask & GDK_WINDOW_STATE_ICONIFIED) &&
            (e->new_window_state & GDK_WINDOW_STATE_ICONIFIED)) {
            t->hide_window();
        }
        return FALSE;
    }

    static gboolean on_tick(gpointer data) {
        if (Impl* t = impl_of(data)) t->refresh();
        return G_SOURCE_CONTINUE;
    }

    void start(void* native_window) {
        window = GTK_WINDOW(native_window);
        if (window) {
            g_object_add_weak_pointer(G_OBJECT(window), reinterpret_cast<gpointer*>(&window));
        }

        if (cairo_surface_t* icon = load_app_icon()) {
            pixmaps = icon_pixmaps(icon);
            // The window's own icon too, for the same reason the pixmaps are
            // pixels: without an installed .desktop file it would have none.
            if (window) {
                if (GdkPixbuf* pb = gdk_pixbuf_get_from_surface(
                        icon, 0, 0, cairo_image_surface_get_width(icon),
                        cairo_image_surface_get_height(icon))) {
                    gtk_window_set_icon(window, pb);
                    g_object_unref(pb);
                }
            }
            cairo_surface_destroy(icon);
        }

        GError* err = nullptr;
        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
        if (!bus) {
            // No session bus, so no panel to show an icon on; minimizing keeps
            // working the way it always has.
            g_clear_error(&err);
            return;
        }
        cancel = g_cancellable_new();

        // Parsed once and kept: the interface info is shared by every
        // registration for the life of the process.
        static GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kIntrospection, nullptr);
        if (!info) return;

        static const GDBusInterfaceVTable item_vtable = {
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*,
               const gchar* method, GVariant*, GDBusMethodInvocation* inv, gpointer data) {
                if (Impl* t = impl_of(data)) t->item_call(method, inv);
                else g_dbus_method_invocation_return_value(inv, nullptr);
            },
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*,
               const gchar* name, GError** error, gpointer data) -> GVariant* {
                Impl* t = impl_of(data);
                GVariant* v = t ? t->item_property(name) : nullptr;
                if (!v) g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                    "No such property: %s", name);
                return v;
            },
            nullptr, {}};
        static const GDBusInterfaceVTable menu_vtable = {
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*,
               const gchar* method, GVariant* params, GDBusMethodInvocation* inv,
               gpointer data) {
                if (Impl* t = impl_of(data)) {
                    t->menu_call(method, params, inv);
                } else {
                    g_dbus_method_invocation_return_dbus_error(
                        inv, "org.freedesktop.DBus.Error.Failed", "Shutting down");
                }
            },
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*,
               const gchar* name, GError** error, gpointer data) -> GVariant* {
                Impl* t = impl_of(data);
                GVariant* v = t ? t->menu_property(name) : nullptr;
                if (!v) g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                    "No such property: %s", name);
                return v;
            },
            nullptr, {}};

        refresh();   // the menu and tooltip have to exist before anyone asks

        item_reg = g_dbus_connection_register_object(
            bus, kItemPath, g_dbus_node_info_lookup_interface(info, kItemIface),
            &item_vtable, link_ref(link), link_unref, nullptr);
        menu_reg = g_dbus_connection_register_object(
            bus, kMenuPath, g_dbus_node_info_lookup_interface(info, kMenuIface),
            &menu_vtable, link_ref(link), link_unref, nullptr);
        if (!item_reg || !menu_reg) return;

        host_sub = g_dbus_connection_signal_subscribe(
            bus, kWatcher, kWatcher, nullptr, kWatcherPath, nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, &on_host_signal, link_ref(link), link_unref);

        // The name the specification asks for: the interface, the pid, and a
        // per-process count -- there is only ever the one.
        bus_name = std::string(kItemIface) + "-" + std::to_string(getpid()) + "-1";
        watch_id = g_bus_watch_name_on_connection(
            bus, kWatcher, G_BUS_NAME_WATCHER_FLAGS_NONE, &on_watcher_appeared,
            &on_watcher_vanished, link_ref(link), link_unref);

        if (window) {
            state_handler = g_signal_connect_data(
                window, "window-state-event", G_CALLBACK(&on_window_state), link_ref(link),
                [](gpointer data, GClosure*) { link_unref(data); }, GConnectFlags(0));
        }
        timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 1000, &on_tick, link_ref(link),
                                   link_unref);

        ready = true;
        refresh();   // takes the name, unless the setting says no icon
    }
};

Tray::Tray(void* window, StatusFn status, CommandFn on_command) : impl_(new Impl) {
    impl_->status = std::move(status);
    impl_->command = std::move(on_command);
    impl_->link = new Link;
    impl_->link->impl = impl_.get();
    impl_->start(window);
}

Tray::~Tray() = default;

bool Tray::supported() { return true; }
void Tray::show_window() { impl_->show_window(); }
void Tray::refresh() { impl_->refresh(); }

}  // namespace transcriptor::app

// ===========================================================================
#else   // macOS, or a build with no native window: nothing to put in a tray
// ===========================================================================

namespace transcriptor::app {

struct Tray::Impl {};

bool Tray::supported() { return false; }
Tray::Tray(void*, StatusFn, CommandFn) {}
Tray::~Tray() = default;
void Tray::show_window() {}
void Tray::refresh() {}

}  // namespace transcriptor::app

#endif
