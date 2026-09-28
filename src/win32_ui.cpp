// win32_ui.cpp — Win32 UI for OLAS (one pane per language).
//
// Layout:
//   banner:   [OLAS] title strip
//   row 1:    [Options v] [Restore] [EN] [buttons]  [ES] [buttons]
//   panes:    one RichEdit per language, side by side
//   status:   bottom status bar
//
// The Options / Restore buttons live in a small dedicated overlay window
// (g_toolbar_overlay) that is a child of the main window. The overlay is
// placed on top of the first docked pane's header row and raised in the
// main window's sibling z-order once per layout.
//
// Options is a popup menu (TrackPopupMenu).
//
// Auto-scroll: each pane tracks follow_tail. When true, new content scrolls
// the RichEdit to the bottom. The user disengages by scrolling up; it
// re-engages when they scroll back to the bottom. The Options menu has an
// "Auto-scroll output" command that forces all panes back to following.
//
// All buttons use a custom class (OLASButton) with rounded corners and
// theme-aware colors, since stock Windows buttons look wrong in dark mode.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef _RICHEDIT_VER
#define _RICHEDIT_VER 0x0500
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <richedit.h>
#include <dwmapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "win32_ui.h"
#include "capture.h"

#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

// ---------------- RichEdit dynamic load ----------------

static HMODULE g_msftedit = nullptr;
static void load_richedit(void) {
    if (!g_msftedit) g_msftedit = LoadLibraryW(L"Msftedit.dll");
}
#define MSFTEDIT_CLASSW L"RICHEDIT50W"

// ---------------- messages ----------------

#define WM_APP_UPDATE       (WM_APP + 1)
#define WM_APP_STATUS       (WM_APP + 2)
#define WM_APP_UPDATE_AVAIL (WM_APP + 3)

// ---------------- IDs ----------------

enum {
    ID_TOGGLE    = 101,
    ID_DECOUPLE  = 102,
    ID_STATUS    = 106,
    ID_EDIT      = 107,
    ID_COLLAPSE  = 108,
    ID_OPTIONS   = 109,
    ID_RESTORE   = 110,

};
// Cap the per-pane transcript log. See olas-gtk.cpp for the same logic.
static constexpr size_t MAX_LOG_LINES = 4000;
static constexpr size_t TRIM_TO_LINES = 2000;

// Popup menu command IDs.
enum {
    IDM_DEVICE_BASE   = 1000,
    IDM_ZOOM_SMALL    = 2001,
    IDM_ZOOM_NORMAL   = 2002,
    IDM_ZOOM_LARGE    = 2003,
    IDM_ZOOM_XL       = 2004,
    IDM_TOGGLE_STAMPS = 3001,
    IDM_THEME_LIGHT   = 4001,
    IDM_THEME_DARK    = 4002,
    IDM_AUTOSCROLL_ON = 5001,
};

// ---------------- theme ----------------

struct Theme {
    COLORREF bg, text, stamp, window_bg, banner_bg, banner_text;
    COLORREF btn_bg, btn_bg_hover, btn_bg_pressed, btn_fg, btn_border, btn_accent;
};

static const Theme THEME_LIGHT = {
    RGB(0xFF,0xFF,0xFF), RGB(0x00,0x00,0x00), RGB(0x77,0x77,0x77),
    RGB(0xF0,0xF0,0xF0), RGB(0x1F,0x6F,0x78), RGB(0xFF,0xFF,0xFF),
    RGB(0xE8,0xE8,0xE8), RGB(0xDD,0xDD,0xDD), RGB(0xCC,0xCC,0xCC),
    RGB(0x20,0x20,0x20), RGB(0xC8,0xC8,0xC8), RGB(0x1F,0x6F,0x78)
};
static const Theme THEME_DARK = {
    RGB(0x1E,0x1E,0x1E), RGB(0xE8,0xE8,0xE8), RGB(0x90,0x90,0x90),
    RGB(0x2A,0x2A,0x2A), RGB(0x14,0x4A,0x50), RGB(0xF0,0xF0,0xF0),
    RGB(0x3A,0x3A,0x3A), RGB(0x4A,0x4A,0x4A), RGB(0x2E,0x2E,0x2E),
    RGB(0xE8,0xE8,0xE8), RGB(0x55,0x55,0x55), RGB(0x1F,0x6F,0x78)
};
static Theme  g_theme            = THEME_DARK;
static bool   g_dark_mode        = true;   // dark by default
static HBRUSH g_window_brush     = nullptr;
static HBRUSH g_banner_brush     = nullptr;
static HWND   g_banner           = nullptr;
static HFONT  g_font_banner      = nullptr;
static HFONT  g_font_label       = nullptr;
static HFONT  g_font_glyph       = nullptr;
static HFONT  g_font_btn         = nullptr;
static int    g_current_device_index = 0;
static UINT     g_dpi = 96;
static int      D(int px);
static HBRUSH   bg_brush(void);

// ---------------- custom button ----------------

enum class BtnKind { Push, Toggle };

struct BtnData {
    BtnKind kind = BtnKind::Push;
    bool    toggled = false;
    bool    hover = false;
    bool    pressed = false;
    std::wstring text;
};

static LRESULT CALLBACK olas_button_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    BtnData *d = (BtnData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_MOUSEMOVE: {
            if (!d) break;
            if (!d->hover) {
                d->hover = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme = { sizeof(tme) };
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE: {
            if (!d) break;
            d->hover = false;
            d->pressed = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!d) break;
            d->pressed = true;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!d) break;
            ReleaseCapture();
            const bool was_pressed = d->pressed;
            d->pressed = false;
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT r; GetClientRect(hwnd, &r);
            const bool inside = PtInRect(&r, pt) != FALSE;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (was_pressed && inside) {
                if (d->kind == BtnKind::Toggle) d->toggled = !d->toggled;
                HWND parent = GetParent(hwnd);
                int id = GetDlgCtrlID(hwnd);
                SendMessageW(parent, WM_COMMAND,
                             MAKEWPARAM(id, BN_CLICKED), (LPARAM)hwnd);
            }
            return 0;
        }
        case WM_SETTEXT: {
            if (d && lp) {
                d->text = (const wchar_t *)lp;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return TRUE;
        }
        case WM_GETTEXT: {
            if (d && lp) {
                size_t n = (size_t)wp;
                const wchar_t *src = d->text.c_str();
                size_t len = wcslen(src);
                if (len >= n) len = n - 1;
                wmemcpy((wchar_t *)lp, src, len);
                ((wchar_t *)lp)[len] = L'\0';
                return (LRESULT)len;
            }
            return 0;
        }
        case WM_GETTEXTLENGTH:
            return d ? (LRESULT)d->text.size() : 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT r; GetClientRect(hwnd, &r);

            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bm = CreateCompatibleBitmap(dc, r.right, r.bottom);
            HBITMAP old_bm = (HBITMAP)SelectObject(mem, bm);

            const bool pressed = d && d->pressed;
            const bool hover   = d && d->hover;
            const bool toggled = d && d->kind == BtnKind::Toggle && d->toggled;

            COLORREF bgc, fgc, brd;
            if (toggled) {
                bgc = g_theme.btn_accent;
                fgc = RGB(0xFF, 0xFF, 0xFF);
                brd = g_theme.btn_accent;
            } else {
                bgc = pressed ? g_theme.btn_bg_pressed
                              : (hover ? g_theme.btn_bg_hover : g_theme.btn_bg);
                fgc = g_theme.btn_fg;
                brd = g_theme.btn_border;
            }

            // Fill client with parent bg (so corners look transparent)
            FillRect(mem, &r, bg_brush());

            const int radius = D(8);
            HRGN rgn = CreateRoundRectRgn(0, 0, r.right + 1, r.bottom + 1,
                                          radius * 2, radius * 2);
            HBRUSH fill = CreateSolidBrush(bgc);
            FillRgn(mem, rgn, fill);
            DeleteObject(fill);
            HBRUSH edge = CreateSolidBrush(brd);
            FrameRgn(mem, rgn, edge, 1, 1);
            DeleteObject(edge);
            DeleteObject(rgn);

            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, fgc);
            HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
            if (!font) font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HFONT old_font = (HFONT)SelectObject(mem, font);

            std::wstring t = d ? d->text : std::wstring();
            RECT tr = r;
            InflateRect(&tr, -D(4), 0);
            DrawTextW(mem, t.c_str(), -1, &tr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(mem, old_font);

            BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old_bm);
            DeleteObject(bm);
            DeleteDC(mem);

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        case WM_NCDESTROY:
            if (d) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                delete d;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND create_button(HWND parent, int id, const wchar_t *text,
                          BtnKind kind, HINSTANCE hInst,
                          int x, int y, int w, int h) {
    HWND btn = CreateWindowExW(0, L"OLASButton", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               x, y, w, h, parent, (HMENU)(INT_PTR)id,
                               hInst, nullptr);
    if (!btn) return nullptr;
    BtnData *d = new BtnData();
    d->kind = kind;
    d->text = text;
    SetWindowLongPtrW(btn, GWLP_USERDATA, (LONG_PTR)d);
    if (g_font_btn) SendMessageW(btn, WM_SETFONT, (WPARAM)g_font_btn, TRUE);
    return btn;
}

static void button_set_text(HWND btn, const wchar_t *text) {
    if (btn) SendMessageW(btn, WM_SETTEXT, 0, (LPARAM)text);
}

static void button_set_toggled(HWND btn, bool on) {
    if (!btn) return;
    BtnData *d = (BtnData *)GetWindowLongPtrW(btn, GWLP_USERDATA);
    if (d) { d->toggled = on; InvalidateRect(btn, nullptr, FALSE); }
}

static bool button_get_toggled(HWND btn) {
    if (!btn) return false;
    BtnData *d = (BtnData *)GetWindowLongPtrW(btn, GWLP_USERDATA);
    return d ? d->toggled : false;
}

// ---------------- state ----------------

struct LogLine {
    std::string prefix;
    std::string body;
    bool        is_error = false;
};

struct Pane {
    int slot = -1;
    bool enabled = true;
    std::string language;
    bool collapsed = false;

    HWND collapse_btn = nullptr;
    HWND container    = nullptr;
    HWND label        = nullptr;
    HWND toggle_btn   = nullptr;
    HWND detach_btn   = nullptr;
    HWND edit         = nullptr;

    HWND float_window = nullptr;

    LONG partial_start = 0;
    std::string last_partial;
    std::vector<LogLine> log;

    // Auto-scroll: true when the pane should follow new output.
    bool follow_tail = true;
    // Set while we programmatically scroll, so the subclass doesn't
    // misinterpret our own scroll as a user scroll.
    bool suppress_scroll_check = false;
};

struct Update {
    int   slot;
    std::string prefix;
    std::string body;
    bool  is_final;
    bool  is_error;
};
struct UpdateAvail {
    std::string local_sha;
    std::string remote_sha;
    std::string url;
};

static HWND        g_main_window      = nullptr;
static HWND        g_toolbar_overlay  = nullptr;
static HWND        g_status           = nullptr;
static HFONT       g_font_mono        = nullptr;
static HFONT       g_font_ui          = nullptr;

static std::vector<std::unique_ptr<Pane>> g_panes;
static int         g_body_pt       = 14;
static bool        g_show_stamps   = true;


static Pane *g_floated_pane       = nullptr;
static Pane *g_collapsed_pane     = nullptr;
static Pane *g_first_docked_pane  = nullptr;
static int   g_toolbar_w          = 0;

static olas_toggle_fn g_on_toggle = nullptr;
static olas_device_fn g_on_device = nullptr;

// Old RichEdit proc for the subclass
static WNDPROC g_edit_old_proc = nullptr;

// ---------------- forward declarations ----------------

static inline int D(int px);
static wchar_t *a2w(const char *s);
static void set_status_w(const wchar_t *w);
static void pane_layout(Pane *p);
static void pane_rerender(Pane *p);
static void pane_set_collapsed(Pane *p, bool collapsed);
static void pane_detach(Pane *p);
static void pane_reattach(Pane *p);
static void pane_create_controls(Pane *p, HINSTANCE hInst);
static void pane_respawn(Pane *p);
static void apply_zoom_to(int pt);
static void apply_theme(void);
static void main_layout(void);
static void show_options_menu(void);
static void raise_toolbar_overlay(void);
static LRESULT CALLBACK overlay_proc(HWND, UINT, WPARAM, LPARAM);

// ---------------- helpers ----------------

static inline int D(int px) { return MulDiv(px, (int)g_dpi, 96); }

static wchar_t *a2w(const char *s) {
    if (!s) return nullptr;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return nullptr;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return nullptr;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void set_status_w(const wchar_t *w) {
    if (g_status) SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)w);
}

static inline HBRUSH bg_brush(void) {
    return g_window_brush ? g_window_brush : (HBRUSH)(COLOR_BTNFACE + 1);
}

// ---------------- RichEdit tagged insertion ----------------

static void re_insert(HWND edit, const wchar_t *text, bool is_stamp,
                      bool is_error) {
    CHARRANGE cr = { -1, -1 };
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&cr);

    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof cf);
    cf.cbSize  = sizeof cf;
    cf.dwMask  = CFM_COLOR | CFM_SIZE;
    cf.yHeight = (LONG)(g_body_pt * 20);
    cf.crTextColor = (is_stamp || is_error) ? g_theme.stamp : g_theme.text;
    cf.dwEffects = 0;
    if (is_stamp) cf.yHeight = (LONG)((g_body_pt - 4) * 20);
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)text);
}

static LONG edit_length(HWND edit) {
    GETTEXTLENGTHEX gtl;
    ZeroMemory(&gtl, sizeof gtl);
    gtl.flags    = GTL_DEFAULT;
    gtl.codepage = 1200;
    return (LONG)SendMessageW(edit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
}

static void edit_delete_from(HWND edit, LONG start) {
    CHARRANGE cr = { start, -1 };
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&cr);
    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)L"");
}

// Scroll the pane's RichEdit to the bottom without triggering the
// user-scroll detector.
static void pane_scroll_to_bottom(Pane *p) {
    if (!p || !p->edit) return;
    p->suppress_scroll_check = true;
    SendMessageW(p->edit, WM_VSCROLL, SB_BOTTOM, 0);
    SendMessageW(p->edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(p->edit, EM_SCROLLCARET, 0, 0);
    p->suppress_scroll_check = false;
}

// Is the RichEdit currently scrolled to (or within tolerance of) the bottom?
static bool edit_is_at_bottom(HWND edit) {
    SCROLLINFO si = { sizeof(si) };
    si.fMask = SIF_ALL;
    if (!GetScrollInfo(edit, SB_VERT, &si)) return true;
    if ((int)si.nMax <= (int)si.nPage) return true;
    const int max_pos = si.nMax - (int)si.nPage;
    return si.nPos >= max_pos - 2;   // small tolerance
}

// ---------------- RichEdit subclass for user-scroll detection ----------------

static LRESULT CALLBACK edit_subclass_proc(HWND hwnd, UINT msg,
                                           WPARAM wp, LPARAM lp) {
    Pane *p = (Pane *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    LRESULT r = CallWindowProcW(g_edit_old_proc, hwnd, msg, wp, lp);

    const bool user_scrolled =
        msg == WM_VSCROLL ||
        msg == WM_MOUSEWHEEL ||
        msg == WM_KEYDOWN ||
        msg == WM_LBUTTONUP ||
        msg == WM_MBUTTONUP;

    if (p && user_scrolled && !p->suppress_scroll_check) {
        // RichEdit may not have updated scroll info yet at this point; ask
        // again on the next idle so we see the post-message position.
        bool at_bottom = edit_is_at_bottom(hwnd);
        p->follow_tail = at_bottom;
    }
    return r;
}

// ---------------- toolbar overlay ----------------

static void raise_toolbar_overlay(void) {
    if (!g_toolbar_overlay) return;
    SetWindowPos(g_toolbar_overlay, HWND_TOP,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: {
            HDC dc = (HDC)wp;
            RECT r; GetClientRect(hwnd, &r);
            FillRect(dc, &r, bg_brush());
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, g_theme.text);
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)bg_brush();
        }
        case WM_COMMAND:
            if (g_main_window)
                SendMessageW(g_main_window, WM_COMMAND, wp, lp);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- pane layout ----------------

static void pane_layout(Pane *p) {
    if (!p || !p->container) return;
    RECT r;
    GetClientRect(p->container, &r);

    const int pad   = D(4);
    const int h_row = D(30);
    const int bh    = D(24);
    const int y     = (h_row - bh) / 2;

    const int bw_dec = D(90);
    const int bw_sm  = D(38);

    int x = r.right - pad - bw_dec;
    MoveWindow(p->detach_btn,   x, y, bw_dec, bh, TRUE);
    x -= bw_sm + D(4);
    MoveWindow(p->collapse_btn, x, y, bw_sm,  bh, TRUE);
    x -= bw_sm + D(4);
    MoveWindow(p->toggle_btn,   x, y, bw_sm,  bh, TRUE);

    int header_left = pad;
    if (p == g_first_docked_pane && !p->collapsed)
        header_left += g_toolbar_w;

    int label_w = x - D(8) - header_left;
    if (label_w < D(20)) label_w = D(20);
    MoveWindow(p->label, header_left, y, label_w, bh, TRUE);

    MoveWindow(p->edit, pad, h_row,
               r.right - 2 * pad,
               r.bottom - h_row - pad, TRUE);
}

// ---------------- pane rendering ----------------

static void pane_rerender(Pane *p) {
    if (!p || !p->edit) return;
    SendMessageW(p->edit, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(p->edit, L"");
    p->partial_start = 0;
    p->last_partial.clear();

    for (const auto &line : p->log) {
        if (line.is_error) {
            std::wstring w = L"[error] ";
            wchar_t *wb = a2w(line.body.c_str());
            if (wb) { w += wb; free(wb); }
            w += L"\n";
            re_insert(p->edit, w.c_str(), false, true);
        } else {
            if (g_show_stamps) {
                wchar_t *wp = a2w(line.prefix.c_str());
                if (wp) { re_insert(p->edit, wp, true, false); free(wp); }
            }
            wchar_t *wb = a2w(line.body.c_str());
            if (wb) { re_insert(p->edit, wb, false, false); free(wb); }
            re_insert(p->edit, L"\n", false, false);
        }
    }

    p->partial_start = edit_length(p->edit);
    SendMessageW(p->edit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(p->edit, nullptr, TRUE);
    if (p->follow_tail) pane_scroll_to_bottom(p);
}

static void pane_apply(Pane *p, const Update &u) {
    if (!p) return;
    HWND e = p->edit;
    if (!e) return;

    SendMessageW(e, WM_SETREDRAW, FALSE, 0);

    if (u.is_error) {
        edit_delete_from(e, p->partial_start);
        p->last_partial.clear();
        std::wstring line = L"[error] ";
        wchar_t *wb = a2w(u.body.c_str());
        if (wb) { line += wb; free(wb); }
        line += L"\n";
        re_insert(e, line.c_str(), false, true);
        p->partial_start = edit_length(e);
        p->log.push_back({ "", u.body, true });
        SendMessageW(e, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(e, nullptr, TRUE);
        if (p->follow_tail) pane_scroll_to_bottom(p);
        return;
    }

    std::string cur = u.prefix + u.body;
    if (!u.is_final) {
        if (cur == p->last_partial) {
            SendMessageW(e, WM_SETREDRAW, TRUE, 0);
            return;
        }
        p->last_partial = cur;
    } else {
        p->last_partial.clear();
    }

    edit_delete_from(e, p->partial_start);

    if (g_show_stamps) {
        wchar_t *wp = a2w(u.prefix.c_str());
        if (wp) { re_insert(e, wp, true, false); free(wp); }
    }
    wchar_t *wb = a2w(u.body.c_str());
    if (wb) { re_insert(e, wb, false, false); free(wb); }

    if (u.is_final) {
        re_insert(e, L"\n", false, false);
        p->partial_start = edit_length(e);
        p->log.push_back({ u.prefix, u.body, false });

        if (p->log.size() > MAX_LOG_LINES) {
            const size_t drop = p->log.size() - TRIM_TO_LINES;
            p->log.erase(p->log.begin(), p->log.begin() + (ptrdiff_t)drop);
            // Rebuild the RichEdit. WM_SETREDRAW is already FALSE, so no
            // flicker reaches the user.
            SetWindowTextW(p->edit, L"");
            p->partial_start = 0;
            for (const auto &line : p->log) {
                if (line.is_error) {
                    std::wstring w = L"[error] ";
                    wchar_t *wb = a2w(line.body.c_str());
                    if (wb) { w += wb; free(wb); }
                    w += L"\n";
                    re_insert(p->edit, w.c_str(), false, true);
                } else {
                    if (g_show_stamps) {
                        wchar_t *wp = a2w(line.prefix.c_str());
                        if (wp) { re_insert(p->edit, wp, true, false); free(wp); }
                    }
                    wchar_t *wb = a2w(line.body.c_str());
                    if (wb) { re_insert(p->edit, wb, false, false); free(wb); }
                    re_insert(p->edit, L"\n", false, false);
                }
            }
            p->partial_start = edit_length(p->edit);
        }

        if (p->follow_tail) pane_scroll_to_bottom(p);
    }

    SendMessageW(e, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(e, nullptr, TRUE);
    if (p->follow_tail) pane_scroll_to_bottom(p);
}

// ---------------- collapse ----------------

static void pane_set_collapsed(Pane *p, bool collapsed) {
    if (!p) return;
    if (collapsed && g_collapsed_pane && g_collapsed_pane != p)
        pane_set_collapsed(g_collapsed_pane, false);

    p->collapsed     = collapsed;
    g_collapsed_pane = collapsed ? p : nullptr;

    if (p->collapse_btn)
        button_set_text(p->collapse_btn, collapsed ? L"\u25B8" : L"\u25BE");
    ShowWindow(p->edit,       collapsed ? SW_HIDE : SW_SHOW);
    ShowWindow(p->toggle_btn, collapsed ? SW_HIDE : SW_SHOW);
    ShowWindow(p->detach_btn, collapsed ? SW_HIDE : SW_SHOW);
    main_layout();
}

// ---------------- decouple / recouple ----------------

static void pane_detach(Pane *p) {
    if (!p || p->float_window) return;
    if (g_floated_pane && g_floated_pane != p) {
        set_status_w(L"Only one pane can be decoupled at a time \u2014 recouple it first.");
        return;
    }
    if (g_collapsed_pane == p) pane_set_collapsed(p, false);

    ShowWindow(p->container, SW_HIDE);
    HWND w = CreateWindowExW(
        WS_EX_TOOLWINDOW, L"OLASFloat", L"OLAS",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, D(640), D(420),
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!w) { ShowWindow(p->container, SW_SHOW); return; }
    SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)p);

    SetParent(p->container, w);
    ShowWindow(p->container, SW_SHOW);
    p->float_window = w;
    g_floated_pane  = p;

    wchar_t title[64];
    wchar_t *wl = a2w(p->language.c_str());
    if (wl) {
        CharUpperW(wl);
        wsprintfW(title, L"OLAS \u2014 %s", wl);
        SetWindowTextW(w, title);
        free(wl);
    } else {
        SetWindowTextW(w, L"OLAS");
    }

    button_set_text(p->detach_btn, L"Recouple");
    for (auto &pp : g_panes)
        if (pp.get() != p) EnableWindow(pp->detach_btn, FALSE);

    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);
    RECT r; GetClientRect(w, &r);
    MoveWindow(p->container, 0, 0, r.right, r.bottom, TRUE);
    pane_layout(p);

    main_layout();
    raise_toolbar_overlay();
}

static void pane_reattach(Pane *p) {
    if (!p || !p->float_window) return;
    HWND w = p->float_window;
    p->float_window = nullptr;
    g_floated_pane  = nullptr;

    SetParent(p->container, g_main_window);
    ShowWindow(p->container, SW_SHOWNA);
    DestroyWindow(w);

    button_set_text(p->detach_btn, L"Decouple");
    for (auto &pp : g_panes) EnableWindow(pp->detach_btn, TRUE);

    main_layout();
    raise_toolbar_overlay();
}

// ---------------- respawn ----------------

static bool pane_alive(Pane *p) {
    return p && p->container && IsWindow(p->container) &&
           p->edit && IsWindow(p->edit);
}

static void pane_respawn(Pane *p) {
    if (!p || pane_alive(p)) return;
    p->float_window = nullptr;
    if (g_floated_pane   == p) g_floated_pane   = nullptr;
    if (g_collapsed_pane == p) g_collapsed_pane = nullptr;
    p->collapsed = false;
    pane_create_controls(p, GetModuleHandleW(nullptr));
    pane_rerender(p);
    main_layout();
    raise_toolbar_overlay();
}

// ---------------- pane creation ----------------

static void pane_create_controls(Pane *p, HINSTANCE hInst) {
    p->container = CreateWindowExW(
        WS_EX_CONTROLPARENT, L"OLASPane", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0, g_main_window, nullptr, hInst, nullptr);
    SetWindowLongPtrW(p->container, GWLP_USERDATA, (LONG_PTR)p);

    p->label = CreateWindowExW(0, L"STATIC", L"",
                               WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                               0, 0, 0, 0, p->container, nullptr, hInst, nullptr);
    {
        wchar_t *wl = a2w(p->language.c_str());
        if (wl) { CharUpperW(wl); SetWindowTextW(p->label, wl); free(wl); }
    }
    SendMessageW(p->label, WM_SETFONT, (WPARAM)g_font_label, TRUE);

    p->toggle_btn = create_button(p->container, ID_TOGGLE, L"\u23F9",
                                  BtnKind::Push, hInst, 0, 0, 0, 0);
    SendMessageW(p->toggle_btn, WM_SETFONT, (WPARAM)g_font_glyph, TRUE);

    p->collapse_btn = create_button(p->container, ID_COLLAPSE, L"\u25BE",
                                    BtnKind::Push, hInst, 0, 0, 0, 0);
    SendMessageW(p->collapse_btn, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

    p->detach_btn = create_button(p->container, ID_DECOUPLE, L"Decouple",
                                  BtnKind::Push, hInst, 0, 0, 0, 0);
    SendMessageW(p->detach_btn, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

    p->edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, MSFTEDIT_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        0, 0, 0, 0, p->container, (HMENU)ID_EDIT, hInst, nullptr);
    SendMessageW(p->edit, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
    SendMessageW(p->edit, EM_SETBKGNDCOLOR, 0, (LPARAM)g_theme.bg);
    SendMessageW(p->edit, EM_SETTARGETDEVICE, 0, 0);

    // Subclass so we can detect user-initiated scrolls.
    SetWindowLongPtrW(p->edit, GWLP_USERDATA, (LONG_PTR)p);
    g_edit_old_proc = (WNDPROC)SetWindowLongPtrW(
        p->edit, GWLP_WNDPROC, (LONG_PTR)edit_subclass_proc);
}

// ---------------- pane window proc ----------------

static LRESULT CALLBACK pane_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Pane *p = (Pane *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_SIZE:
            if (p) pane_layout(p);
            return 0;

        case WM_ERASEBKGND: {
            HDC dc = (HDC)wp;
            RECT rc; GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, bg_brush());
            return 1;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, g_theme.text);
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)bg_brush();
        }

        case WM_COMMAND: {
            if (!p) break;
            switch (LOWORD(wp)) {
                case ID_TOGGLE:
                    if (g_on_toggle) g_on_toggle(p->slot);
                    return 0;
                case ID_DECOUPLE:
                    if (p->float_window) pane_reattach(p);
                    else                 pane_detach(p);
                    return 0;
                case ID_COLLAPSE:
                    pane_set_collapsed(p, !p->collapsed);
                    return 0;
            }
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- floating window proc ----------------

static LRESULT CALLBACK float_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Pane *p = (Pane *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_SIZE:
            if (p && p->container)
                MoveWindow(p->container, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
            return 0;

        case WM_ERASEBKGND: {
            HDC dc = (HDC)wp;
            RECT rc; GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, bg_brush());
            return 1;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, g_theme.text);
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)bg_brush();
        }

        case WM_EXITSIZEMOVE: {
            if (!p) return 0;
            RECT floatR, mainR, inter;
            GetWindowRect(hwnd, &floatR);
            GetWindowRect(g_main_window, &mainR);
            if (IntersectRect(&inter, &floatR, &mainR)) {
                LONG interArea = (inter.right - inter.left) * (inter.bottom - inter.top);
                LONG floatArea = (floatR.right - floatR.left) * (floatR.bottom - floatR.top);
                if (floatArea > 0 && interArea * 2 >= floatArea)
                    pane_reattach(p);
            }
            return 0;
        }
        case WM_CLOSE:
            if (p) pane_reattach(p);
            return 0;
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- banner ----------------

static LRESULT CALLBACK banner_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);

        HBRUSH br = g_banner_brush ? g_banner_brush
                                   : CreateSolidBrush(g_theme.banner_bg);
        FillRect(dc, &r, br);
        if (!g_banner_brush) DeleteObject(br);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_theme.banner_text);
        HFONT old = (HFONT)SelectObject(dc, g_font_banner);
        DrawTextW(dc, L"OLAS", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- zoom / theme ----------------

static void apply_zoom_to(int pt) {
    if (pt < 8)  pt = 8;
    if (pt > 32) pt = 32;
    if (pt == g_body_pt && g_font_mono) return;
    g_body_pt = pt;

    if (g_font_mono) { DeleteObject(g_font_mono); g_font_mono = nullptr; }
    g_font_mono = CreateFontW(
        -MulDiv(g_body_pt, (int)g_dpi, 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!g_font_mono) g_font_mono = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    for (auto &pp : g_panes) {
        if (pp->edit)
            SendMessageW(pp->edit, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
        pane_rerender(pp.get());
    }
}

static void apply_theme(void) {
    g_theme = g_dark_mode ? THEME_DARK : THEME_LIGHT;

    HBRUSH new_window = CreateSolidBrush(g_theme.window_bg);
    HBRUSH new_banner = CreateSolidBrush(g_theme.banner_bg);

    HBRUSH old_window = g_window_brush;
    HBRUSH old_banner = g_banner_brush;
    g_window_brush = new_window;
    g_banner_brush = new_banner;

    for (auto &pp : g_panes) {
        if (!pp->edit) continue;
        SendMessageW(pp->edit, EM_SETBKGNDCOLOR, 0, (LPARAM)g_theme.bg);
        pane_rerender(pp.get());
    }

    if (g_main_window) {
        COLORREF cap_bg   = g_theme.banner_bg;
        COLORREF cap_text = RGB(0xFF, 0xFF, 0xFF);
        DwmSetWindowAttribute(g_main_window, DWMWA_CAPTION_COLOR, &cap_bg, sizeof(cap_bg));
        DwmSetWindowAttribute(g_main_window, DWMWA_TEXT_COLOR,   &cap_text, sizeof(cap_text));
    }

    // Force every custom button to repaint with the new palette.
    for (auto &pp : g_panes) {
        InvalidateRect(pp->toggle_btn,   nullptr, TRUE);
        InvalidateRect(pp->collapse_btn, nullptr, TRUE);
        InvalidateRect(pp->detach_btn,   nullptr, TRUE);
    }
    if (g_toolbar_overlay) {
        EnumChildWindows(g_toolbar_overlay, [](HWND child, LPARAM) -> BOOL {
            InvalidateRect(child, nullptr, TRUE);
            return TRUE;
        }, 0);
    }

    if (g_main_window) {
        RedrawWindow(g_main_window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    if (g_banner) {
        RedrawWindow(g_banner, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
    if (g_toolbar_overlay) {
        RedrawWindow(g_toolbar_overlay, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    if (old_window) DeleteObject(old_window);
    if (old_banner) DeleteObject(old_banner);
}

// ---------------- options popup menu ----------------

static void show_options_menu(void) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    HMENU dev_menu = CreatePopupMenu();
    int n = capture_device_count();
    int dev_added = 0;
    for (int i = 0; i < n; ++i) {
        const char *name = capture_device_name(i);
        if (!name || !*name) continue;
        wchar_t *w = a2w(name);
        if (!w) continue;
        UINT flags = MF_STRING;
        if (i == g_current_device_index) flags |= MF_CHECKED;
        AppendMenuW(dev_menu, flags, (UINT_PTR)(IDM_DEVICE_BASE + i), w);
        free(w);
        ++dev_added;
    }
    if (dev_added == 0)
        AppendMenuW(dev_menu, MF_STRING | MF_GRAYED, 0, L"(no capture devices)");
    AppendMenuW(menu, MF_POPUP | MF_STRING, (UINT_PTR)dev_menu, L"Capture device");

    HMENU zoom_menu = CreatePopupMenu();
    AppendMenuW(zoom_menu, MF_STRING | (g_body_pt == 10 ? MF_CHECKED : 0),
                IDM_ZOOM_SMALL,  L"Small");
    AppendMenuW(zoom_menu, MF_STRING | (g_body_pt == 12 ? MF_CHECKED : 0),
                IDM_ZOOM_NORMAL, L"Normal");
    AppendMenuW(zoom_menu, MF_STRING | (g_body_pt == 14 ? MF_CHECKED : 0),
                IDM_ZOOM_LARGE,  L"Large");
    AppendMenuW(zoom_menu, MF_STRING | (g_body_pt == 18 ? MF_CHECKED : 0),
                IDM_ZOOM_XL,     L"Extra large");
    AppendMenuW(menu, MF_POPUP | MF_STRING, (UINT_PTR)zoom_menu, L"Zoom");

    AppendMenuW(menu, MF_STRING | (g_show_stamps ? MF_CHECKED : 0),
                IDM_TOGGLE_STAMPS, L"Show timestamps");

    // Auto-scroll. Checked when every pane is following.
    bool all_follow = true;
    for (auto &pp : g_panes) if (!pp->follow_tail) { all_follow = false; break; }
    AppendMenuW(menu, MF_STRING | (all_follow ? MF_CHECKED : 0),
                IDM_AUTOSCROLL_ON, L"Auto-scroll output");

    HMENU theme_menu = CreatePopupMenu();
    AppendMenuW(theme_menu, MF_STRING | (!g_dark_mode ? MF_CHECKED : 0),
                IDM_THEME_LIGHT, L"Light");
    AppendMenuW(theme_menu, MF_STRING | ( g_dark_mode ? MF_CHECKED : 0),
                IDM_THEME_DARK,  L"Dark");
    AppendMenuW(menu, MF_POPUP | MF_STRING, (UINT_PTR)theme_menu, L"Appearance");

    HWND btn = g_toolbar_overlay ? GetDlgItem(g_toolbar_overlay, ID_OPTIONS) : nullptr;
    RECT br = {0};
    if (btn) GetWindowRect(btn, &br);
    else     GetWindowRect(g_main_window, &br);

    int cmd = (int)TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
        br.left, br.bottom, 0, g_main_window, nullptr);
    DestroyMenu(menu);

    if (cmd >= IDM_DEVICE_BASE && cmd < IDM_DEVICE_BASE + 1000) {
        int idx = cmd - IDM_DEVICE_BASE;
        g_current_device_index = idx;
        if (g_on_device) g_on_device(idx);
    } else if (cmd >= IDM_ZOOM_SMALL && cmd <= IDM_ZOOM_XL) {
        int pt = (cmd == IDM_ZOOM_SMALL)  ? 10
               : (cmd == IDM_ZOOM_NORMAL) ? 12
               : (cmd == IDM_ZOOM_LARGE)  ? 14
               :                            18;
        apply_zoom_to(pt);
    } else if (cmd == IDM_TOGGLE_STAMPS) {
        g_show_stamps = !g_show_stamps;
        for (auto &pp : g_panes) pane_rerender(pp.get());
    } else if (cmd == IDM_AUTOSCROLL_ON) {
        // Force every pane back to following, then jump to the bottom.
        for (auto &pp : g_panes) {
            pp->follow_tail = true;
            pane_scroll_to_bottom(pp.get());
        }
    } else if (cmd == IDM_THEME_LIGHT || cmd == IDM_THEME_DARK) {
        bool dark = (cmd == IDM_THEME_DARK);
        if (dark != g_dark_mode) { g_dark_mode = dark; apply_theme(); }
    }
}

// ---------------- main window layout ----------------

static void main_layout(void) {
    RECT r;
    GetClientRect(g_main_window, &r);
    const int pad      = D(8);
    const int gap      = D(6);
    const int h_row    = D(30);
    const int banner_h = D(36);

    if (g_banner) MoveWindow(g_banner, 0, 0, r.right, banner_h, TRUE);

    int status_h = 0;
    if (g_status) {
        RECT sr; GetWindowRect(g_status, &sr);
        status_h = sr.bottom - sr.top;
    }

    const int toolbar_y = banner_h + pad;
    const int bw_opts   = D(100);
    const int bw_rst    = D(88);
    const int ovl_x     = pad + D(4);
    const int ovl_w     = bw_opts + gap + bw_rst;
    const int btns_w    = D(4) + ovl_w + D(8);

    const int pane_x0 = pad;
    const int pane_y  = toolbar_y;
    int pane_h = r.bottom - pane_y - pad - status_h;
    if (pane_h < D(60)) pane_h = D(60);

    int n_docked = 0;
    g_first_docked_pane = nullptr;
    for (auto &pp : g_panes) {
        if (pp->container && !pp->float_window) {
            ++n_docked;
            if (!g_first_docked_pane) g_first_docked_pane = pp.get();
        }
    }
    g_toolbar_w = btns_w;

    if (n_docked == 0) {
        if (g_toolbar_overlay)
            MoveWindow(g_toolbar_overlay, ovl_x, toolbar_y, ovl_w, h_row, TRUE);
        raise_toolbar_overlay();
        return;
    }

    int total_w = r.right - pane_x0 - pad - (n_docked - 1) * gap;
    if (total_w < D(100)) total_w = D(100);

    const int collapsed_w = D(160);
    const bool any_collapsed = (g_collapsed_pane != nullptr) &&
                               !g_collapsed_pane->float_window &&
                               (n_docked > 1);

    int px = pane_x0;
    for (auto &pp : g_panes) {
        Pane *p = pp.get();
        if (!p->container || p->float_window) continue;
        int w;
        if (any_collapsed) {
            w = (p == g_collapsed_pane) ? collapsed_w : (total_w - collapsed_w);
        } else {
            w = total_w / n_docked;
        }
        MoveWindow(p->container, px, pane_y, w, pane_h, TRUE);
        px += w + gap;
    }

    if (g_toolbar_overlay) {
        MoveWindow(g_toolbar_overlay, ovl_x, toolbar_y, ovl_w, h_row, TRUE);
        raise_toolbar_overlay();
    }
}

// ---------------- main window proc ----------------

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            main_layout();
            return 0;

        case WM_ERASEBKGND: {
            HDC dc = (HDC)wp;
            RECT rc; GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, bg_brush());
            return 1;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, g_theme.text);
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)bg_brush();
        }

        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            RECT *r = (RECT *)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top,
                         r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            main_layout();
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_OPTIONS:
                    show_options_menu();
                    return 0;
                case ID_RESTORE:
                    for (auto &pp : g_panes) pane_respawn(pp.get());
                    return 0;
            }
            return 0;

        case WM_APP_UPDATE: {
            Update *u = (Update *)lp;
            if (u) {
                if (u->slot >= 0 && u->slot < (int)g_panes.size())
                    pane_apply(g_panes[u->slot].get(), *u);
                delete u;
            }
            return 0;
        }

        case WM_APP_STATUS: {
            wchar_t *w = (wchar_t *)lp;
            if (w) { set_status_w(w); free(w); }
            return 0;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO *m = (MINMAXINFO *)lp;
            // Big enough for two panes + toolbar + buttons without clipping.
            m->ptMinTrackSize.x = D(900);
            m->ptMinTrackSize.y = D(500);
            return 0;
        }

        case WM_CLOSE:
            for (auto &pp : g_panes)
                if (pp->float_window) pane_reattach(pp.get());
            PostQuitMessage(0);
            return 0;

        case WM_APP_UPDATE_AVAIL: {
            UpdateAvail *ua = (UpdateAvail *)lp;
            if (ua) {
                std::wstring msg =
                    L"A newer version of OLAS is available.\n\n"
                    L"  Your build:    ";
                { wchar_t buf[32]; wsprintfW(buf, L"%.12s", ua->local_sha.c_str()); msg += buf; }
                msg += L"\n  Latest (main): ";
                { wchar_t buf[32]; wsprintfW(buf, L"%.12s", ua->remote_sha.c_str()); msg += buf; }
                msg += L"\n\nOpen the project page in your browser "
                       L"to download the update?";

                int r = MessageBoxW(g_main_window, msg.c_str(),
                                    L"Update Available",
                                    MB_YESNO | MB_ICONINFORMATION);
                if (r == IDYES && !ua->url.empty()) {
                    wchar_t *url_w = a2w(ua->url.c_str());
                    if (url_w) {
                        ShellExecuteW(nullptr, L"open", url_w,
                                      nullptr, nullptr, SW_SHOWNORMAL);
                        free(url_w);
                    }
                }
                delete ua;
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- class registration ----------------

static void register_classes(HINSTANCE hInst) {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);

    wc.lpfnWndProc   = main_proc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"OLASMain";
    RegisterClassExW(&wc);

    wc.lpfnWndProc   = pane_proc;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"OLASPane";
    RegisterClassExW(&wc);

    wc.lpfnWndProc   = float_proc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"OLASFloat";
    RegisterClassExW(&wc);

    wc.lpfnWndProc   = banner_proc;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"OLASBanner";
    RegisterClassExW(&wc);

    wc.lpfnWndProc   = overlay_proc;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"OLASToolbarOverlay";
    RegisterClassExW(&wc);

    wc.lpfnWndProc   = olas_button_proc;
    wc.hbrBackground = nullptr;
    wc.hCursor       = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = L"OLASButton";
    RegisterClassExW(&wc);
}

// ---------------- public API ----------------

int win32_ui_init(const std::vector<std::string> &languages) {
    load_richedit();

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    #if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
    #else
    SetProcessDPIAware();
    #endif

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof icc;
    icc.dwICC  = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    register_classes(hInst);

    g_main_window = CreateWindowExW(
        0, L"OLASMain", L"OLAS \u2014 Open Local Audio Scribe (Windows)",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, D(1280), D(720),
        nullptr, nullptr, hInst, nullptr);
    if (!g_main_window) return 0;

    g_dpi = GetDpiForWindow(g_main_window);
    if (g_dpi == 0) g_dpi = 96;

    /* Fonts */
    g_font_mono = CreateFontW(
        -MulDiv(g_body_pt, (int)g_dpi, 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!g_font_mono) g_font_mono = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_font_ui = CreateFontW(
        -MulDiv(10, (int)g_dpi, 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    g_font_btn = CreateFontW(
        -MulDiv(10, (int)g_dpi, 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    g_font_label = CreateFontW(
        -MulDiv(11, (int)g_dpi, 72),
        0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    g_font_glyph = CreateFontW(
        -MulDiv(14, (int)g_dpi, 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI Symbol");

    g_font_banner = CreateFontW(
        -MulDiv(20, (int)g_dpi, 72),
        0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    /* Theme brushes */
    g_theme = g_dark_mode ? THEME_DARK : THEME_LIGHT;
    g_window_brush = CreateSolidBrush(g_theme.window_bg);
    g_banner_brush = CreateSolidBrush(g_theme.banner_bg);

    /* Win11 caption tint (no-op on Win10). */
    {
        COLORREF cap_bg   = g_theme.banner_bg;
        COLORREF cap_text = RGB(0xFF, 0xFF, 0xFF);
        DwmSetWindowAttribute(g_main_window, DWMWA_CAPTION_COLOR, &cap_bg, sizeof(cap_bg));
        DwmSetWindowAttribute(g_main_window, DWMWA_TEXT_COLOR,   &cap_text, sizeof(cap_text));
    }

    /* Banner */
    g_banner = CreateWindowExW(0, L"OLASBanner", L"",
                               WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                               g_main_window, nullptr, hInst, nullptr);

    /* Toolbar overlay */
    const int bw_opts = D(100);
    const int bw_rst  = D(88);
    const int gap     = D(6);
    const int h_row   = D(30);

    g_toolbar_overlay = CreateWindowExW(
        0, L"OLASToolbarOverlay", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0, g_main_window, nullptr, hInst, nullptr);

    create_button(g_toolbar_overlay, ID_OPTIONS, L"Options \u25BE",
                  BtnKind::Push, hInst, 0, 0, bw_opts, h_row);
    create_button(g_toolbar_overlay, ID_RESTORE, L"Restore",
                  BtnKind::Push, hInst, bw_opts + gap, 0, bw_rst, h_row);

    g_status = CreateWindowExW(
        0, STATUSCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, g_main_window, (HMENU)ID_STATUS, hInst, nullptr);
    set_status_w(L"ready");

    /* Panes */
    for (size_t i = 0; i < languages.size(); ++i) {
        auto p = std::make_unique<Pane>();
        p->slot     = (int)i;
        p->language = languages[i];
        pane_create_controls(p.get(), hInst);
        g_panes.push_back(std::move(p));
    }

    ShowWindow(g_main_window, SW_SHOW);
    UpdateWindow(g_main_window);
    main_layout();
    raise_toolbar_overlay();

    return 1;
}

void win32_ui_populate_devices(int default_index) {
    int n = capture_device_count();
    if (default_index >= 0 && default_index < n)
        g_current_device_index = default_index;
    else if (n > 0)
        g_current_device_index = 0;
}

void win32_ui_run(olas_toggle_fn on_toggle, olas_device_fn on_device) {
    g_on_toggle = on_toggle;
    g_on_device = on_device;

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void win32_ui_post_update(int slot, const char *prefix, const char *body,
                          int is_final, int is_error) {
    if (!g_main_window) return;
    Update *u = new Update();
    u->slot     = slot;
    u->prefix   = prefix ? prefix : "";
    u->body     = body   ? body   : "";
    u->is_final = is_final != 0;
    u->is_error = is_error != 0;
    PostMessageW(g_main_window, WM_APP_UPDATE, 0, (LPARAM)u);
}

void win32_ui_post_status(const char *text) {
    if (!g_main_window || !text) return;
    wchar_t *w = a2w(text);
    if (!w) return;
    PostMessageW(g_main_window, WM_APP_STATUS, 0, (LPARAM)w);
}

void win32_ui_set_pane_enabled(int slot, int enabled) {
    if (slot < 0 || slot >= (int)g_panes.size()) return;
    Pane *p = g_panes[slot].get();
    p->enabled = enabled != 0;
    button_set_text(p->toggle_btn, p->enabled ? L"\u23F9" : L"\u25B6");
}

void win32_ui_show_update_prompt(const char* local_sha,
                                 const char* remote_sha,
                                 const char* url)
{
    if (!g_main_window) return;
    UpdateAvail *ua = new UpdateAvail();
    ua->local_sha  = local_sha  ? local_sha  : "";
    ua->remote_sha = remote_sha ? remote_sha : "";
    ua->url        = url        ? url        : "";
    PostMessageW(g_main_window, WM_APP_UPDATE_AVAIL, 0, (LPARAM)ua);
}

void win32_ui_shutdown(void) {
    if (g_main_window) {
        DestroyWindow(g_main_window);
        g_main_window = nullptr;
    }
    g_toolbar_overlay = nullptr;
    g_panes.clear();
    if (g_font_mono)   { DeleteObject(g_font_mono);   g_font_mono   = nullptr; }
    if (g_font_label)  { DeleteObject(g_font_label);  g_font_label  = nullptr; }
    if (g_font_glyph)  { DeleteObject(g_font_glyph);  g_font_glyph  = nullptr; }
    if (g_font_banner) { DeleteObject(g_font_banner); g_font_banner = nullptr; }
    if (g_font_btn)    { DeleteObject(g_font_btn);    g_font_btn    = nullptr; }
    if (g_font_ui)     { DeleteObject(g_font_ui);     g_font_ui     = nullptr; }
    if (g_window_brush){ DeleteObject(g_window_brush);g_window_brush= nullptr; }
    if (g_banner_brush){ DeleteObject(g_banner_brush);g_banner_brush= nullptr; }
}
