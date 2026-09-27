// win32_ui.cpp — Win32 UI for OLAS (one pane per language).
//
// Layout:
//   toolbar: [Device:] [combo] [Stamps] [A-] [A+]
//   panes:   one per language, split 50/50 (or full width if only one)
//   status:  bottom status bar
//
// Each pane:
//   [lang] [Start/Stop] [Detach]
//   RichEdit (read-only, monospace, color tags, resizable font)
//
// Updates from worker threads arrive via PostMessage(WM_APP_UPDATE); the
// payload is a heap-allocated Update* freed by the UI thread.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "win32_ui.h"
#include "capture.h"

// ---------------- RichEdit dynamic load ----------------

typedef HRESULT (WINAPI *PFN_CreateTextServices)(HWND, void*, void*);
static HMODULE g_msftedit = nullptr;

static void load_richedit(void) {
    if (!g_msftedit) g_msftedit = LoadLibraryW(L"Msftedit.dll");
}

#define MSFTEDIT_CLASSW L"RICHEDIT50W"

// ---------------- messages ----------------

#define WM_APP_UPDATE (WM_APP + 1)
#define WM_APP_STATUS (WM_APP + 2)

// ---------------- IDs ----------------

enum {
    ID_DEVICE   = 100,
    ID_TOGGLE   = 101,
    ID_DETACH   = 102,
    ID_STAMPS   = 103,
    ID_ZOOM_IN  = 104,
    ID_ZOOM_OUT = 105,
    ID_STATUS   = 106,
    ID_EDIT     = 107,
};

// ---------------- state ----------------

// One committed line of transcript kept in memory so that font/stamp toggles
// can replay the visible scrollback instead of wiping the pane.
struct LogLine {
    std::string prefix;
    std::string body;
    bool        is_error = false;
};

struct Pane {
    int slot = -1;
    bool enabled = true;
    std::string language;

    HWND container = nullptr;
    HWND label     = nullptr;
    HWND toggle_btn= nullptr;
    HWND detach_btn= nullptr;
    HWND edit      = nullptr;   // RichEdit

    HWND float_window = nullptr;  // non-null when detached

    LONG partial_start = 0;       // RichEdit char position of partial start
    std::string last_partial;     // dedupe
    std::vector<LogLine> log;     // finalized lines (errors included)
};

struct Update {
    int   slot;
    std::string prefix;
    std::string body;
    bool  is_final;
    bool  is_error;
};

static HWND        g_main_window   = nullptr;
static HWND        g_device_combo  = nullptr;
static HWND        g_stamps_btn    = nullptr;
static HWND        g_status        = nullptr;
static HFONT       g_font_mono     = nullptr;
static HFONT       g_font_ui       = nullptr;
static HBRUSH      g_bk_brush      = nullptr;

static std::vector<std::unique_ptr<Pane>> g_panes;
static int         g_body_pt       = 14;   // base body font size (points)
static bool        g_show_stamps   = true;
static UINT        g_dpi           = 96;

static olas_toggle_fn g_on_toggle  = nullptr;
static olas_device_fn g_on_device  = nullptr;

// Forward decls (used by pane_proc).
static void pane_detach(Pane *p);
static void pane_reattach(Pane *p);

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

// ---------------- RichEdit tagged insertion ----------------

static void re_insert(HWND edit, const wchar_t *text, bool is_stamp,
                      bool is_error, bool is_hidden) {
    CHARRANGE cr = { -1, -1 };
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&cr);

    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR | CFM_SIZE | CFM_HIDDEN;
    cf.yHeight = (LONG)(g_body_pt * 20);   // twips (1pt = 20twips)
    cf.crTextColor = (is_stamp || is_error)
                     ? RGB(0x88, 0x88, 0x88)
                     : GetSysColor(COLOR_WINDOWTEXT);
    cf.dwEffects = 0;
    if (is_stamp && is_hidden) cf.dwEffects |= CFE_HIDDEN;
    if (is_stamp)              cf.yHeight = (LONG)((g_body_pt - 4) * 20);
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)text);
}

static LONG edit_length(HWND edit) {
    GETTEXTLENGTHEX gtl;
    ZeroMemory(&gtl, sizeof gtl);
    gtl.flags = GTL_DEFAULT;
    gtl.codepage = 1200;
    return (LONG)SendMessageW(edit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
}

static void edit_delete_from(HWND edit, LONG start) {
    CHARRANGE cr = { start, -1 };
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&cr);
    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)L"");
}

static void edit_scroll_bottom(HWND edit) {
    SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);
}

// ---------------- pane rendering ----------------

static void pane_layout(Pane *p) {
    if (!p->container) return;
    RECT r;
    GetClientRect(p->container, &r);
    const int pad = D(4), hdr = D(28);

    MoveWindow(p->label, pad, D(6), D(60), D(20), TRUE);

    int bw = D(76), bh = D(22);
    int x = r.right - pad - bw;
    MoveWindow(p->detach_btn, x, D(3), bw, bh, TRUE);
    x -= bw + D(4);
    MoveWindow(p->toggle_btn, x, D(3), bw, bh, TRUE);

    MoveWindow(p->edit, pad, hdr, r.right - 2 * pad, r.bottom - hdr - pad, TRUE);
}

// Re-render the entire pane from the in-memory log.  Used on Stamps/zoom
// toggles so that changing presentation does not erase the scrollback.
static void pane_rerender(Pane *p) {
    if (!p->edit) return;
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
            re_insert(p->edit, w.c_str(), false, true, false);
        } else {
            wchar_t *wp = a2w(line.prefix.c_str());
            wchar_t *wb = a2w(line.body.c_str());
            if (wp) { re_insert(p->edit, wp, true, false, !g_show_stamps); free(wp); }
            if (wb) { re_insert(p->edit, wb, false, false, false); free(wb); }
            re_insert(p->edit, L"\n", false, false, false);
        }
    }

    p->partial_start = edit_length(p->edit);
    SendMessageW(p->edit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(p->edit, nullptr, TRUE);
    edit_scroll_bottom(p->edit);
}

static void pane_apply(Pane *p, const Update &u) {
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
        re_insert(e, line.c_str(), false, true, false);
        p->partial_start = edit_length(e);
        p->log.push_back({ "", u.body, true });
        edit_scroll_bottom(e);
        SendMessageW(e, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(e, nullptr, TRUE);
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

    wchar_t *wp = a2w(u.prefix.c_str());
    wchar_t *wb = a2w(u.body.c_str());

    if (wp) {
        re_insert(e, wp, /*stamp=*/true, false, !g_show_stamps);
        free(wp);
    }
    if (wb) {
        re_insert(e, wb, false, false, false);
        free(wb);
    }

    if (u.is_final) {
        re_insert(e, L"\n", false, false, false);
        p->partial_start = edit_length(e);
        p->log.push_back({ u.prefix, u.body, false });
        edit_scroll_bottom(e);
    }

    SendMessageW(e, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(e, nullptr, TRUE);
}

// ---------------- pane window proc ----------------

static LRESULT CALLBACK pane_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Pane *p = (Pane *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_SIZE:
            if (p) pane_layout(p);
            return 0;

        case WM_COMMAND: {
            if (!p) break;
            switch (LOWORD(wp)) {
                case ID_TOGGLE:
                    if (g_on_toggle) g_on_toggle(p->slot);
                    return 0;
                case ID_DETACH:
                    if (p->float_window) pane_reattach(p);
                    else                 pane_detach(p);
                    return 0;
            }
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- detach / reattach ----------------

void pane_detach(Pane *p) {
    if (p->float_window) return;

    ShowWindow(p->container, SW_HIDE);

    HWND w = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        L"OLASFloat", L"OLAS",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, D(640), D(420),
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!w) {
        ShowWindow(p->container, SW_SHOW);
        return;
    }
    SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)p);

    SetParent(p->container, w);
    ShowWindow(p->container, SW_SHOW);
    p->float_window = w;

    wchar_t title[64];
    wchar_t *wl = a2w(p->language.c_str());
    if (wl) {
        wsprintfW(title, L"OLAS — %s", wl);
        SetWindowTextW(w, title);
        free(wl);
    } else {
        SetWindowTextW(w, L"OLAS");
    }

    SetWindowTextW(p->detach_btn, L"Dock");
    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);

    RECT r; GetClientRect(w, &r);
    MoveWindow(p->container, 0, 0, r.right, r.bottom, TRUE);
    pane_layout(p);
}

void pane_reattach(Pane *p) {
    if (!p->float_window) return;
    HWND w = p->float_window;
    p->float_window = nullptr;

    SetParent(p->container, g_main_window);
    ShowWindow(p->container, SW_SHOW);
    DestroyWindow(w);

    SetWindowTextW(p->detach_btn, L"Detach");

    RECT r; GetClientRect(g_main_window, &r);
    SendMessageW(g_main_window, WM_SIZE, 0, MAKELPARAM(r.right, r.bottom));
}

// ---------------- floating window proc ----------------

static LRESULT CALLBACK float_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Pane *p = (Pane *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_SIZE:
            if (p && p->container) {
                MoveWindow(p->container, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
            }
            return 0;
        case WM_CLOSE:
            if (p) pane_reattach(p);
            return 0;
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- main window layout ----------------

static void main_layout() {
    RECT r;
    GetClientRect(g_main_window, &r);
    const int pad = D(8), h_row = D(28);

    int status_h = 0;
    if (g_status) {
        RECT sr; GetWindowRect(g_status, &sr);
        status_h = sr.bottom - sr.top;
    }

    // Toolbar
    int x = pad, y = pad;
    MoveWindow(GetDlgItem(g_main_window, 900), x, y + D(4), D(46), D(20), TRUE);
    x += D(50);
    MoveWindow(g_device_combo, x, y, D(260), D(220), TRUE);
    x += D(270);
    MoveWindow(g_stamps_btn, x, y, D(80), h_row, TRUE);
    x += D(90);
    MoveWindow(GetDlgItem(g_main_window, ID_ZOOM_OUT), x, y, D(40), h_row, TRUE);
    x += D(44);
    MoveWindow(GetDlgItem(g_main_window, ID_ZOOM_IN),  x, y, D(40), h_row, TRUE);

    // Panes
    int pane_y = y + h_row + pad;
    int pane_h = r.bottom - pane_y - pad - status_h;
    if (pane_h < D(60)) pane_h = D(60);

    int n = (int)g_panes.size();
    if (n == 0) return;
    int gap = D(4);
    int total_w = r.right - 2 * pad - (n - 1) * gap;
    int each    = total_w / n;
    int extra   = total_w - each * n;

    int px = pad;
    for (int i = 0; i < n; ++i) {
        Pane *p = g_panes[i].get();
        if (!p->container) continue;
        int w = each + (i < extra ? 1 : 0);
        if (!p->float_window) {
            MoveWindow(p->container, px, pane_y, w, pane_h, TRUE);
        }
        px += w + gap;
    }
}

// ---------------- main window proc ----------------

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            main_layout();
            return 0;

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
                case ID_DEVICE:
                    if (HIWORD(wp) == CBN_SELCHANGE && g_on_device) {
                        int idx = (int)SendMessageW(g_device_combo, CB_GETCURSEL, 0, 0);
                        if (idx != CB_ERR) g_on_device(idx);
                    }
                    return 0;
                case ID_STAMPS:
                    g_show_stamps = !g_show_stamps;
                    for (auto &pp : g_panes) pane_rerender(pp.get());
                    return 0;
                case ID_ZOOM_IN:
                    if (g_body_pt < 32) {
                        g_body_pt += 2;
                        // Re-apply font to every edit, then replay.
                        for (auto &pp : g_panes) {
                            if (g_font_mono) DeleteObject(g_font_mono);
                            g_font_mono = CreateFontW(
                                -MulDiv(g_body_pt, (int)g_dpi, 72),
                                0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");
                            SendMessageW(pp->edit, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
                            pane_rerender(pp.get());
                        }
                    }
                    return 0;
                case ID_ZOOM_OUT:
                    if (g_body_pt > 8) {
                        g_body_pt -= 2;
                        for (auto &pp : g_panes) {
                            if (g_font_mono) DeleteObject(g_font_mono);
                            g_font_mono = CreateFontW(
                                -MulDiv(g_body_pt, (int)g_dpi, 72),
                                0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");
                            SendMessageW(pp->edit, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
                            pane_rerender(pp.get());
                        }
                    }
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
            m->ptMinTrackSize.x = D(640);
            m->ptMinTrackSize.y = D(400);
            return 0;
        }

        case WM_CLOSE:
            for (auto &pp : g_panes) if (pp->float_window) pane_reattach(pp.get());
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- public API ----------------

int win32_ui_init(const std::vector<std::string> &languages) {
    load_richedit();

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    // Per-monitor DPI awareness (v2), fall back to legacy system-DPI.
#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();
    }
#else
    SetProcessDPIAware();
#endif

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof icc;
    icc.dwICC  = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc; ZeroMemory(&wc, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = main_proc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"OLASMain";
    if (!RegisterClassExW(&wc)) return 0;

    wc.lpfnWndProc   = pane_proc;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"OLASPane";
    if (!RegisterClassExW(&wc)) return 0;

    wc.lpfnWndProc   = float_proc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"OLASFloat";
    if (!RegisterClassExW(&wc)) return 0;

    g_main_window = CreateWindowExW(
        0, L"OLASMain", L"OLAS — Open Local Audio Scribe (Windows)",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, D(1280), D(720),
        nullptr, nullptr, hInst, nullptr);
    if (!g_main_window) return 0;

    g_dpi = GetDpiForWindow(g_main_window);
    if (g_dpi == 0) g_dpi = 96;

    g_font_mono = CreateFontW(
        -MulDiv(g_body_pt, (int)g_dpi, 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!g_font_mono) g_font_mono = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_font_ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_bk_brush = (HBRUSH)(COLOR_WINDOW + 1);

    // ---- toolbar controls ----
    HWND lbl = CreateWindowExW(0, L"STATIC", L"Device:",
        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
        g_main_window, (HMENU)900, hInst, nullptr);
    SendMessageW(lbl, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

    g_device_combo = CreateWindowExW(
        0, L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, g_main_window, (HMENU)ID_DEVICE, hInst, nullptr);
    SendMessageW(g_device_combo, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

    g_stamps_btn = CreateWindowExW(
        0, L"BUTTON", L"Stamps",
        BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0, g_main_window, (HMENU)ID_STAMPS, hInst, nullptr);
    SendMessageW(g_stamps_btn, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

    HWND zo = CreateWindowExW(0, L"BUTTON", L"A-",
        BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, g_main_window, (HMENU)ID_ZOOM_OUT, hInst, nullptr);
    HWND zi = CreateWindowExW(0, L"BUTTON", L"A+",
        BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, g_main_window, (HMENU)ID_ZOOM_IN, hInst, nullptr);
    SendMessageW(zo, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    SendMessageW(zi, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

    g_status = CreateWindowExW(
        0, STATUSCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, g_main_window, (HMENU)ID_STATUS, hInst, nullptr);
    set_status_w(L"ready");

    // ---- panes ----
    for (size_t i = 0; i < languages.size(); ++i) {
        auto p = std::make_unique<Pane>();
        p->slot = (int)i;
        p->language = languages[i];

        p->container = CreateWindowExW(
            WS_EX_CONTROLPARENT, L"OLASPane", L"",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, g_main_window, nullptr, hInst, nullptr);
        SetWindowLongPtrW(p->container, GWLP_USERDATA, (LONG_PTR)p.get());

        p->label = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, p->container, nullptr, hInst, nullptr);
        wchar_t *wl = a2w(p->language.c_str());
        if (wl) { SetWindowTextW(p->label, wl); free(wl); }
        SendMessageW(p->label, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

        p->toggle_btn = CreateWindowExW(0, L"BUTTON", L"Stop",
            BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0, 0, 0, 0, p->container, (HMENU)ID_TOGGLE, hInst, nullptr);
        SendMessageW(p->toggle_btn, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

        p->detach_btn = CreateWindowExW(0, L"BUTTON", L"Detach",
            BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0, 0, 0, 0, p->container, (HMENU)ID_DETACH, hInst, nullptr);
        SendMessageW(p->detach_btn, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

        p->edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, MSFTEDIT_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
            0, 0, 0, 0, p->container, (HMENU)ID_EDIT, hInst, nullptr);
        SendMessageW(p->edit, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
        SendMessageW(p->edit, EM_SETBKGNDCOLOR, 0, (LPARAM)GetSysColor(COLOR_WINDOW));
        SendMessageW(p->edit, EM_SETTARGETDEVICE, 0, 0);

        g_panes.push_back(std::move(p));
    }

    ShowWindow(g_main_window, SW_SHOW);
    UpdateWindow(g_main_window);
    main_layout();
    return 1;
}

void win32_ui_populate_devices(int default_index) {
    if (!g_device_combo) return;
    int n = capture_device_count();
    for (int i = 0; i < n; ++i) {
        const char *name = capture_device_name(i);
        if (!name || !*name) continue;
        wchar_t *w = a2w(name);
        if (!w) continue;
        int idx = (int)SendMessageW(g_device_combo, CB_ADDSTRING, 0, (LPARAM)w);
        SendMessageW(g_device_combo, CB_SETITEMDATA, idx, (LPARAM)i);
        free(w);
    }
    if (default_index >= 0 && default_index < n)
        SendMessageW(g_device_combo, CB_SETCURSEL, default_index, 0);
    else if (n > 0)
        SendMessageW(g_device_combo, CB_SETCURSEL, 0, 0);
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
    u->slot = slot;
    u->prefix = prefix ? prefix : "";
    u->body   = body   ? body   : "";
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
    SetWindowTextW(p->toggle_btn, p->enabled ? L"Stop" : L"Start");
}

void win32_ui_shutdown() {
    if (g_main_window) {
        DestroyWindow(g_main_window);
        g_main_window = nullptr;
    }
    g_panes.clear();
    if (g_font_mono) { DeleteObject(g_font_mono); g_font_mono = nullptr; }
}