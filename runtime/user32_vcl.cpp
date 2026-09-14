// The cooperative USER32 model used by windowed compiler frameworks. Handles
// name runtime-owned state; callbacks always re-enter through guest_call.
#include "user32_internal.h"
#include "memory.h"
#include "gdi_image.h"
#include <algorithm>
#include <set>
#include <cstring>

void sched_checkpoint();
namespace user32 {
namespace {
uint32_t active = 0, focus = 0, capture = 0;
struct Timer {
    uint32_t hwnd, id, interval, due, callback, thread;
};
std::map<std::pair<uint32_t, uint32_t>, Timer> timers;
uint32_t next_timer = 1;
std::set<uint32_t> destroying;
} // namespace
void window_created(uint32_t hwnd) {
    if (!active)
        active = hwnd;
    if (!focus)
        focus = hwnd;
}
// Queue each expired timer at most once. Signed subtraction handles DWORD
// clock wrap; no host thread mutates guest memory or invokes a callback here.
void pump_window_timers() {
    uint32_t now = host_millis();
    for (auto &kv : timers) {
        Timer &t = kv.second;
        if (t.thread != guest_current_thread_id() || int32_t(now - t.due) < 0)
            continue;
        bool queued = false;
        for (const auto &m : queue())
            if (m.message == 0x113 && m.hwnd == t.hwnd && m.wparam == t.id) {
                queued = true;
                break;
            }
        if (!queued)
            host_post_message(t.hwnd, 0x113, t.id, t.callback);
        t.due = now + t.interval;
    }
}
// Snapshot children before callbacks: WM_DESTROY/WM_NCDESTROY can recursively
// destroy another window, change parentage or create a new window.
bool destroy_window(X86 *c, uint32_t hwnd) {
    if (!find_window(hwnd) || hwnd == desktop_handle || destroying.count(hwnd))
        return false;
    destroying.insert(hwnd);
    host_dispatch_to_wndproc(c, hwnd, 2, 0, 0);
    std::vector<uint32_t> children;
    for (const auto &kv : windows())
        if (kv.second.parent == hwnd)
            children.push_back(kv.first);
    for (uint32_t child : children)
        destroy_window(c, child);
    host_dispatch_to_wndproc(c, hwnd, 0x82, 0, 0);
    windows().erase(hwnd);
    for (auto i = timers.begin(); i != timers.end();)
        if (i->second.hwnd == hwnd)
            i = timers.erase(i);
        else
            ++i;
    auto &q = queue();
    q.erase(std::remove_if(q.begin(), q.end(), [&](const Msg &m) { return m.hwnd == hwnd; }),
            q.end());
    if (active == hwnd)
        active = 0;
    if (focus == hwnd)
        focus = 0;
    if (capture == hwnd)
        capture = 0;
    if (g_main_hwnd == hwnd)
        g_main_hwnd = windows().empty() ? 0 : windows().begin()->first;
    destroying.erase(hwnd);
    return true;
}
void client_origin(uint32_t hwnd, int32_t *x, int32_t *y) {
    *x = *y = 0;
    std::set<uint32_t> seen;
    while (hwnd && seen.insert(hwnd).second) {
        Window *w = find_window(hwnd);
        if (!w)
            break;
        *x += w->x;
        *y += w->y;
        hwnd = w->parent;
    }
}
bool is_child(uint32_t parent, uint32_t hwnd) {
    if (!parent || parent == hwnd)
        return false;
    Window *w = find_window(hwnd);
    while (w && w->parent) {
        if (w->parent == parent)
            return true;
        w = find_window(w->parent);
    }
    return false;
}
} // namespace user32

namespace {
using namespace user32;
void yes(X86 *c) {
    set_eax(c, 1);
}
void zero(X86 *c) {
    set_eax(c, 0);
}
void set_timer(X86 *c) {
    uint32_t hwnd = arg(c, 0), id = arg(c, 1), interval = std::clamp(arg(c, 2), 10u, 0x7fffffffu);
    if (hwnd && !find_window(hwnd)) {
        set_eax(c, 0);
        return;
    }
    if (!hwnd && (!id || !timers.count({0, id}))) {
        while (timers.count({0, next_timer}))
            ++next_timer;
        id = next_timer++;
    }
    timers[{hwnd, id}] = {
        hwnd, id, interval, host_millis() + interval, arg(c, 3), guest_current_thread_id()};
    set_eax(c, id ? id : 1);
}
void kill_timer(X86 *c) {
    set_eax(c, timers.erase({arg(c, 0), arg(c, 1)}) != 0);
}
std::string prop_key(uint32_t p) {
    return p < 0x10000 ? "#atom" + std::to_string(p) : gm_wstr(p);
}
void get_prop(X86 *c) {
    Window *w = find_window(arg(c, 0));
    auto key = prop_key(arg(c, 1));
    set_eax(c, w && w->props.count(key) ? w->props[key] : 0);
}
void set_prop(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (w)
        w->props[prop_key(arg(c, 1))] = arg(c, 2);
    set_eax(c, w != nullptr);
}
void remove_prop(X86 *c) {
    Window *w = find_window(arg(c, 0));
    auto key = prop_key(arg(c, 1));
    uint32_t old = w && w->props.count(key) ? w->props[key] : 0;
    if (w)
        w->props.erase(key);
    set_eax(c, old);
}
void get_parent(X86 *c) {
    Window *w = find_window(arg(c, 0));
    set_eax(c, w ? (w->parent ? w->parent : w->owner) : 0);
}
void set_parent(X86 *c) {
    Window *w = find_window(arg(c, 0));
    uint32_t parent = arg(c, 1);
    if (!w || (parent && !find_window(parent)) || parent == w->hwnd || is_child(w->hwnd, parent)) {
        set_last_error(87);
        set_eax(c, 0);
        return;
    }
    uint32_t old = w->parent;
    w->parent = parent == desktop_handle ? 0 : parent;
    set_eax(c, old);
}
void child_of(X86 *c) {
    set_eax(c, is_child(arg(c, 0), arg(c, 1)));
}
// Enumerations snapshot handles and revalidate each entry after guest callbacks.
void enumerate_windows(X86 *c, int kind) {
    uint32_t filter = kind ? arg(c, 0) : 0, cb = arg(c, kind ? 1 : 0), param = arg(c, kind ? 2 : 1);
    std::vector<uint32_t> list;
    for (const auto &kv : windows()) {
        const auto &w = kv.second;
        if (kind == 1 ? (!filter || is_child(filter, w.hwnd))
                      : (!w.parent && (kind != 2 || w.thread == filter)))
            list.push_back(w.hwnd);
    }
    bool ok = cb != 0;
    for (uint32_t hwnd : list)
        if (ok && find_window(hwnd))
            ok = guest_call(c, cb, hwnd, param) != 0;
    set_eax(c, ok);
}
void enum_windows(X86 *c) {
    enumerate_windows(c, 0);
}
void enum_children(X86 *c) {
    enumerate_windows(c, 1);
}
void enum_thread(X86 *c) {
    enumerate_windows(c, 2);
}
std::vector<uint32_t> siblings(uint32_t parent) {
    std::vector<uint32_t> list;
    for (const auto &kv : windows())
        if (kv.second.parent == parent)
            list.push_back(kv.first);
    return list;
}
void top_window(X86 *c) {
    auto list = siblings(arg(c, 0) == desktop_handle ? 0 : arg(c, 0));
    set_eax(c, list.empty() ? 0 : list.back());
}
void get_window(X86 *c) {
    Window *w = find_window(arg(c, 0));
    uint32_t kind = arg(c, 1), result = 0;
    if (w) {
        if (kind == 4)
            result = w->owner;
        else {
            auto list = siblings(kind == 5 ? (w->hwnd == desktop_handle ? 0 : w->hwnd) : w->parent);
            auto i = std::find(list.begin(), list.end(), w->hwnd);
            if (!list.empty())
                switch (kind) {
                case 0:
                case 5:
                    result = list.back();
                    break;
                case 1:
                    result = list.front();
                    break;
                case 2:
                    if (i != list.begin() && i != list.end())
                        result = *--i;
                    break;
                case 3:
                    if (i != list.end() && ++i != list.end())
                        result = *i;
                    break;
                }
        }
    }
    set_eax(c, result);
}
void window_thread(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (arg(c, 1))
        wr32(arg(c, 1), w ? 1 : 0);
    set_eax(c, w ? w->thread : 0);
}
void is_window(X86 *c) {
    set_eax(c, find_window(arg(c, 0)) != nullptr);
}
void is_visible(X86 *c) {
    Window *w = find_window(arg(c, 0));
    bool visible = w != nullptr;
    while (w) {
        visible = visible && w->visible;
        w = find_window(w->parent);
    }
    set_eax(c, visible);
}
void is_enabled(X86 *c) {
    Window *w = find_window(arg(c, 0));
    set_eax(c, w && w->enabled);
}
void enable_window(X86 *c) {
    Window *w = find_window(arg(c, 0));
    bool disabled = w && !w->enabled;
    if (w) {
        w->enabled = arg(c, 1) != 0;
        if (w->enabled)
            w->style &= ~0x08000000u;
        else
            w->style |= 0x08000000u;
    }
    set_eax(c, disabled);
}
void iconic(X86 *c) {
    Window *w = find_window(arg(c, 0));
    set_eax(c, w && (w->style & 0x20000000u));
}
void zoomed(X86 *c) {
    Window *w = find_window(arg(c, 0));
    set_eax(c, w && (w->style & 0x01000000u));
}
void get_placement(X86 *c) {
    Window *w = find_window(arg(c, 0));
    uint32_t p = arg(c, 1);
    if (!w || !p || !gm_valid(p, 44) || rd32(p) != 44) {
        set_eax(c, 0);
        return;
    }
    memset(g_mem + p + 4, 0, 40);
    wr32(p + 8, w->show_cmd);
    wr32(p + 28, w->x);
    wr32(p + 32, w->y);
    wr32(p + 36, w->x + w->w);
    wr32(p + 40, w->y + w->h);
    set_eax(c, 1);
}
void set_placement(X86 *c) {
    Window *w = find_window(arg(c, 0));
    uint32_t p = arg(c, 1);
    if (!w || !p || !gm_valid(p, 44) || rd32(p) != 44) {
        set_eax(c, 0);
        return;
    }
    uint32_t hwnd = w->hwnd, cmd = rd32(p + 8);
    w->x = int32_t(rd32(p + 28));
    w->y = int32_t(rd32(p + 32));
    w->w = int32_t(rd32(p + 36)) - w->x;
    w->h = int32_t(rd32(p + 40)) - w->y;
    guest_call(c, imports_resolve("USER32.dll", "ShowWindow"), hwnd, cmd);
    set_eax(c, 1);
}
void get_active(X86 *c) {
    set_eax(c, active);
}
void set_active(X86 *c) {
    uint32_t hwnd = arg(c, 0), old = active;
    if (!hwnd || find_window(hwnd))
        active = hwnd;
    set_eax(c, old);
}
void foreground(X86 *c) {
    if (find_window(arg(c, 0))) {
        active = arg(c, 0);
        set_eax(c, 1);
    } else
        set_eax(c, 0);
}
void get_focus(X86 *c) {
    set_eax(c, focus);
}
void set_focus(X86 *c) {
    uint32_t hwnd = arg(c, 0), old = focus;
    if (!hwnd || find_window(hwnd))
        focus = hwnd;
    set_eax(c, old);
}
void get_capture(X86 *c) {
    set_eax(c, capture);
}
void set_capture(X86 *c) {
    uint32_t old = capture;
    if (find_window(arg(c, 0)))
        capture = arg(c, 0);
    set_eax(c, old);
}
void release_capture(X86 *c) {
    capture = 0;
    set_eax(c, 1);
}
void window_at_point(X86 *c) {
    int32_t x = int32_t(arg(c, 0)), y = int32_t(arg(c, 1));
    uint32_t result = 0;
    for (auto i = windows().rbegin(); i != windows().rend(); ++i) {
        const auto &w = i->second;
        int32_t wx, wy;
        client_origin(w.hwnd, &wx, &wy);
        if (w.visible && w.enabled && x >= wx && y >= wy && int64_t(x) < int64_t(wx) + w.w &&
            int64_t(y) < int64_t(wy) + w.h) {
            result = w.hwnd;
            break;
        }
    }
    set_eax(c, result);
}
void desktop(X86 *c) {
    set_eax(c, desktop_handle);
}
void map_points(X86 *c) {
    int32_t x1, y1, x2, y2;
    client_origin(arg(c, 0), &x1, &y1);
    client_origin(arg(c, 1), &x2, &y2);
    int32_t dx = x1 - x2, dy = y1 - y2;
    uint32_t p = arg(c, 2), n = arg(c, 3);
    if (!p || n > 0x100000 || !gm_valid(p, n * 8)) {
        set_eax(c, 0);
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        wr32(p + i * 8, rd32(p + i * 8) + dx);
        wr32(p + i * 8 + 4, rd32(p + i * 8 + 4) + dy);
    }
    set_eax(c, (uint32_t(uint16_t(dy)) << 16) | uint16_t(dx));
}
void intersect_rect(X86 *c) {
    uint32_t out = arg(c, 0), a = arg(c, 1), b = arg(c, 2);
    if (!out || !a || !b || !gm_valid(out, 16) || !gm_valid(a, 16) || !gm_valid(b, 16)) {
        set_eax(c, 0);
        return;
    }
    int32_t r[4];
    for (uint32_t i = 0; i < 4; ++i)
        r[i] = i < 2 ? std::max(int32_t(rd32(a + 4 * i)), int32_t(rd32(b + 4 * i)))
                     : std::min(int32_t(rd32(a + 4 * i)), int32_t(rd32(b + 4 * i)));
    bool ok = r[0] < r[2] && r[1] < r[3];
    for (uint32_t i = 0; i < 4; ++i)
        wr32(out + 4 * i, ok ? r[i] : 0);
    set_eax(c, ok);
}
void monitor_from(X86 *c, int kind) {
    uint32_t flags = arg(c, kind == 1 ? 2 : 1);
    int32_t l = 0, t = 0, r = 0, b = 0;
    bool valid = true;
    if (kind == 0) {
        Window *w = find_window(arg(c, 0));
        valid = w != nullptr;
        if (w) {
            client_origin(w->hwnd, &l, &t);
            r = l + w->w;
            b = t + w->h;
        }
    } else if (kind == 1) {
        l = int32_t(arg(c, 0));
        t = int32_t(arg(c, 1));
        r = l + 1;
        b = t + 1;
    } else {
        uint32_t p = arg(c, 0);
        valid = p && gm_valid(p, 16);
        if (valid) {
            l = int32_t(rd32(p));
            t = int32_t(rd32(p + 4));
            r = int32_t(rd32(p + 8));
            b = int32_t(rd32(p + 12));
        }
    }
    uint32_t w = 1024, h = 768, bpp = 32;
    ddraw_display_mode(&w, &h, &bpp);
    set_eax(c,
            (flags & 3) || (valid && l < int32_t(w) && t < int32_t(h) && r > 0 && b > 0) ? 1 : 0);
}
void monitor_window(X86 *c) {
    monitor_from(c, 0);
}
void monitor_point(X86 *c) {
    monitor_from(c, 1);
}
void monitor_rect(X86 *c) {
    monitor_from(c, 2);
}
void enum_monitors(X86 *c) {
    uint32_t dc = arg(c, 0), clip = arg(c, 1), cb = arg(c, 2), data = arg(c, 3),
             rect = heap_alloc(16, true);
    if (!cb || !rect) {
        set_eax(c, 0);
        return;
    }
    display_rect(rect);
    bool intersects = true;
    if (clip && gm_valid(clip, 16)) {
        for (uint32_t i = 0; i < 4; ++i)
            wr32(rect + 4 * i,
                 i < 2 ? std::max(int32_t(rd32(rect + 4 * i)), int32_t(rd32(clip + 4 * i)))
                       : std::min(int32_t(rd32(rect + 4 * i)), int32_t(rd32(clip + 4 * i))));
        intersects = int32_t(rd32(rect)) < int32_t(rd32(rect + 8)) &&
                     int32_t(rd32(rect + 4)) < int32_t(rd32(rect + 12));
    }
    uint32_t result = intersects ? guest_call(c, cb, 1, dc, rect, data) : 1;
    heap_free(rect);
    set_eax(c, result);
}
void get_dc(X86 *c) {
    alias_ansi(c, "GetDC");
}
void redraw(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (w) {
        if (arg(c, 3) & 1)
            w->update_pending = true;
        if (arg(c, 3) & 8)
            w->update_pending = false;
        if (arg(c, 3) & 0x100)
            alias_ansi(c, "UpdateWindow");
    }
    set_eax(c, w != nullptr);
}
void scroll_window(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (w)
        w->update_pending = true;
    set_eax(c, w != nullptr);
}
void get_cursor(X86 *c) {
    set_eax(c, g_cursor);
}
void keyboard_state(X86 *c) {
    uint32_t p = arg(c, 0);
    bool ok = p && gm_valid(p, 256);
    if (ok)
        memcpy(g_mem + p, g_key_state, 256);
    set_eax(c, ok);
}
void layout_list(X86 *c) {
    uint32_t n = arg(c, 0), p = arg(c, 1);
    if (n && p && gm_valid(p, 4))
        wr32(p, 0x04090409);
    set_eax(c, 1);
}
void activate_layout(X86 *c) {
    set_eax(c, 0x04090409);
}
void pump_once(X86 *c) {
    host_pump_timers(c);
    pump_window_timers();
    if (g_message_waiter)
        g_message_waiter();
    sched_checkpoint();
}
void wait_message(X86 *c) {
    pump_once(c);
    set_eax(c, 1);
}
void message_wait(X86 *c) {
    pump_window_timers();
    if (!queue().empty()) {
        set_eax(c, arg(c, 0));
        return;
    }
    pump_once(c);
    set_eax(c, 0x102);
}
void last_popup(X86 *c) {
    uint32_t hwnd = arg(c, 0), found = hwnd;
    for (const auto &kv : windows())
        if (kv.second.owner == hwnd && kv.second.visible)
            found = kv.first;
    set_eax(c, found);
}
void ctrl_id(X86 *c) {
    Window *w = find_window(arg(c, 0));
    set_eax(c, w ? w->id : 0);
}
// COLORREF byte order (00BBGGRR), classic system palette, indices 0..29.
const uint32_t colors[30] = {0xc8c8c8, 0,        0xd1b499, 0xdbcdbf, 0xf0f0f0, 0xffffff,
                             0x646464, 0,        0,        0,        0xb4b4b4, 0xf4f7fc,
                             0xababab, 0xff9933, 0xffffff, 0xf0f0f0, 0xa0a0a0, 0x6d6d6d,
                             0,        0,        0xffffff, 0x696969, 0xe3e3e3, 0,
                             0xe1ffff, 0,        0xcc6600, 0xead1b9, 0xf2e4d7, 0xff9933};
void sys_color(X86 *c) {
    uint32_t i = arg(c, 0);
    set_eax(c, i < 30 ? colors[i] : 0);
}
const ImportShim shims[] = {
#define U(name, n, fn) {"USER32.dll", name, n, fn}
    U("SetTimer", 4, set_timer),
    U("KillTimer", 2, kill_timer),
    U("GetPropW", 2, get_prop),
    U("SetPropW", 3, set_prop),
    U("RemovePropW", 2, remove_prop),
    U("SetParent", 2, set_parent),
    U("GetParent", 1, get_parent),
    U("IsChild", 2, child_of),
    U("EnumChildWindows", 3, enum_children),
    U("EnumThreadWindows", 3, enum_thread),
    U("EnumWindows", 2, enum_windows),
    U("GetTopWindow", 1, top_window),
    U("GetWindow", 2, get_window),
    U("GetWindowThreadProcessId", 2, window_thread),
    U("IsWindow", 1, is_window),
    U("IsWindowVisible", 1, is_visible),
    U("IsWindowEnabled", 1, is_enabled),
    U("EnableWindow", 2, enable_window),
    U("IsZoomed", 1, zoomed),
    U("IsIconic", 1, iconic),
    U("GetWindowPlacement", 2, get_placement),
    U("SetWindowPlacement", 2, set_placement),
    U("SetForegroundWindow", 1, foreground),
    U("GetForegroundWindow", 0, get_active),
    U("SetActiveWindow", 1, set_active),
    U("GetActiveWindow", 0, get_active),
    U("SetFocus", 1, set_focus),
    U("GetFocus", 0, get_focus),
    U("GetCapture", 0, get_capture),
    U("SetCapture", 1, set_capture),
    U("ReleaseCapture", 0, release_capture),
    U("WindowFromPoint", 2, window_at_point),
    U("MonitorFromWindow", 2, monitor_window),
    U("MonitorFromPoint", 3, monitor_point),
    U("MonitorFromRect", 2, monitor_rect),
    U("EnumDisplayMonitors", 4, enum_monitors),
    U("GetDesktopWindow", 0, desktop),
    U("GetDCEx", 3, get_dc),
    U("GetWindowDC", 1, get_dc),
    U("RedrawWindow", 4, redraw),
    U("SetWindowRgn", 3, yes),
    U("ScrollWindow", 5, scroll_window),
    U("MapWindowPoints", 4, map_points),
    U("IntersectRect", 3, intersect_rect),
    U("GetCursor", 0, get_cursor),
    U("GetMessageExtraInfo", 0, zero),
    U("GetKeyboardState", 1, keyboard_state),
    U("GetKeyboardLayoutList", 2, layout_list),
    U("ActivateKeyboardLayout", 2, activate_layout),
    U("MsgWaitForMultipleObjects", 5, message_wait),
    U("MsgWaitForMultipleObjectsEx", 5, message_wait),
    U("WaitMessage", 0, wait_message),
    U("GetSysColor", 1, sys_color),
    U("ShowOwnedPopups", 2, yes),
    U("GetLastActivePopup", 1, last_popup),
    U("MessageBeep", 1, yes),
    U("GetDlgCtrlID", 1, ctrl_id),
    U("TranslateMDISysAccel", 2, zero),
    U("ShowCaret", 1, yes),
    U("HideCaret", 1, yes),
#undef U
};
} // namespace
void user32_vcl_register() {
    imports_register(shims, sizeof(shims) / sizeof(shims[0]));
}
