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
// main window's sibling z-order once per layout. Because the overlay never
// moves relative to the panes and is a single sibling that we re-raise,
// detaching/reattaching panes can no longer push the buttons behind a pane.
//
// The first docked, non-collapsed pane indents its language label to make
// room for the overlay. Detached panes are excluded from main_layout(), so
// the surviving docked pane expands to fill the window.
//
// Options is a popup menu (TrackPopupMenu) — no slide-down panel.
//
// Updates from worker threads arrive via PostMessage(WM_APP_UPDATE); the
// payload is a heap-allocated Update* freed by the UI thread.

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

// Popup menu command IDs.
enum {
    IDM_DEVICE_BASE   = 1000,   // 1000..1999 → capture device indices
    IDM_ZOOM_SMALL    = 2001,
    IDM_ZOOM_NORMAL   = 2002,
    IDM_ZOOM_LARGE    = 2003,
    IDM_ZOOM_XL       = 2004,
    IDM_TOGGLE_STAMPS = 3001,
    IDM_THEME_LIGHT   = 4001,
    IDM_THEME_DARK    = 4002,
};

// ---------------- theme ----------------

struct Theme {
    COLORREF bg, text, stamp, window_bg, banner_bg, banner_text;
};
static const Theme THEME_LIGHT = {
    RGB(0xFF,0xFF,0xFF), RGB(0x00,0x00,0x00), RGB(0x77,0x77,0x77),
    RGB(0xF0,0xF0,0xF0), RGB(0x1F,0x6F,0x78), RGB(0xFF,0xFF,0xFF)
};
static const Theme THEME_DARK = {
    RGB(0x1E,0x1E,0x1E), RGB(0xE8,0xE8,0xE8), RGB(0x90,0x90,0x90),
    RGB(0x2A,0x2A,0x2A), RGB(0x14,0x4A,0x50), RGB(0xF0,0xF0,0xF0)
};
static Theme  g_theme            = THEME_LIGHT;
static bool   g_dark_mode        = false;
static HBRUSH g_window_brush     = nullptr;
static HBRUSH g_banner_brush     = nullptr;
static HWND   g_banner           = nullptr;
static HFONT  g_font_banner      = nullptr;
static HFONT  g_font_label       = nullptr;
static HFONT  g_font_glyph       = nullptr;
static int    g_current_device_index = 0;

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
static HWND        g_toolbar_overlay  = nullptr;   // hosts Options / Restore
static HWND        g_status           = nullptr;
static HFONT       g_font_mono        = nullptr;
static HFONT       g_font_ui          = nullptr;

static std::vector<std::unique_ptr<Pane>> g_panes;
static int         g_body_pt       = 14;
static bool        g_show_stamps   = true;
static UINT        g_dpi           = 96;

static Pane *g_floated_pane       = nullptr;
static Pane *g_collapsed_pane     = nullptr;
static Pane *g_first_docked_pane  = nullptr;   // first pane not in a float
static int   g_toolbar_w          = 0;         // width reserved for [Options][Restore]

static olas_toggle_fn g_on_toggle = nullptr;
static olas_device_fn g_on_device = nullptr;

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

                      static void edit_scroll_bottom(HWND edit) {
                          SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);
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
                                  /* Forward button clicks up to the main window, which owns the
                                   * ID_OPTIONS / ID_RESTORE command handlers. */
                                  if (g_main_window)
                                      SendMessageW(g_main_window, WM_COMMAND, wp, lp);
                              return 0;
                          }
                          return DefWindowProcW(hwnd, msg, wp, lp);
                      }

                      // ---------------- pane layout ----------------
                      //
                      // Single header row inside the pane container: language label + inline
                      // [toggle] [collapse] [decouple] group. The RichEdit fills the space below.
                      // The first docked, non-collapsed pane indents its label left edge so it
                      // does not sit under the toolbar overlay the main window draws on top.

                      static void pane_layout(Pane *p) {
                          if (!p || !p->container) return;
                          RECT r;
                          GetClientRect(p->container, &r);

                          const int pad   = D(4);
                          const int h_row = D(28);
                          const int bh    = D(22);
                          const int y     = (h_row - bh) / 2;

                          const int bw_dec = D(84);
                          const int bw_sm  = D(34);

                          // Right-aligned button group: [⏹] [▾] [Decouple]
                          int x = r.right - pad - bw_dec;
                          MoveWindow(p->detach_btn,   x, y, bw_dec, bh, TRUE);
                          x -= bw_sm + D(4);
                          MoveWindow(p->collapse_btn, x, y, bw_sm,  bh, TRUE);
                          x -= bw_sm + D(4);
                          MoveWindow(p->toggle_btn,   x, y, bw_sm,  bh, TRUE);

                          // Left edge: only the first docked, non-collapsed pane reserves room
                          // for the Options / Restore overlay the main window draws on top of it.
                          int header_left = pad;
                          if (p == g_first_docked_pane && !p->collapsed)
                              header_left += g_toolbar_w;

                          int label_w = x - D(8) - header_left;
                          if (label_w < D(20)) label_w = D(20);
                          MoveWindow(p->label, header_left, y, label_w, bh, TRUE);

                          // RichEdit spans the full pane width — no indent here.
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
                          edit_scroll_bottom(p->edit);
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
                              edit_scroll_bottom(e);
                          }

                          SendMessageW(e, WM_SETREDRAW, TRUE, 0);
                          InvalidateRect(e, nullptr, TRUE);
                      }

                      // ---------------- collapse ----------------

                      static void pane_set_collapsed(Pane *p, bool collapsed) {
                          if (!p) return;
                          if (collapsed && g_collapsed_pane && g_collapsed_pane != p)
                              pane_set_collapsed(g_collapsed_pane, false);

                          p->collapsed     = collapsed;
                          g_collapsed_pane = collapsed ? p : nullptr;

                          if (p->collapse_btn)
                              SetWindowTextW(p->collapse_btn, collapsed ? L"\u25B8" : L"\u25BE");
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

                          SetWindowTextW(p->detach_btn, L"Recouple");
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
                          ShowWindow(p->container, SW_SHOWNA);   // show without raising
                          DestroyWindow(w);

                          SetWindowTextW(p->detach_btn, L"Decouple");
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

                          p->toggle_btn = CreateWindowExW(0, L"BUTTON", L"\u23F9",
                                                          BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                          0, 0, 0, 0, p->container, (HMENU)ID_TOGGLE, hInst, nullptr);
                          SendMessageW(p->toggle_btn, WM_SETFONT, (WPARAM)g_font_glyph, TRUE);

                          p->collapse_btn = CreateWindowExW(0, L"BUTTON", L"\u25BE",
                                                            BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                            0, 0, 0, 0, p->container, (HMENU)ID_COLLAPSE, hInst, nullptr);
                          SendMessageW(p->collapse_btn, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

                          p->detach_btn = CreateWindowExW(0, L"BUTTON", L"Decouple",
                                                          BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                          0, 0, 0, 0, p->container, (HMENU)ID_DECOUPLE, hInst, nullptr);
                          SendMessageW(p->detach_btn, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

                          p->edit = CreateWindowExW(
                              WS_EX_CLIENTEDGE, MSFTEDIT_CLASSW, L"",
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                              0, 0, 0, 0, p->container, (HMENU)ID_EDIT, hInst, nullptr);
                          SendMessageW(p->edit, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
                          SendMessageW(p->edit, EM_SETBKGNDCOLOR, 0, (LPARAM)g_theme.bg);
                          SendMessageW(p->edit, EM_SETTARGETDEVICE, 0, 0);
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

                          // Capture device submenu
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

                          // Zoom submenu
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

                          // Timestamps toggle
                          AppendMenuW(menu, MF_STRING | (g_show_stamps ? MF_CHECKED : 0),
                                      IDM_TOGGLE_STAMPS, L"Show timestamps");

                          // Appearance submenu
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
                          } else if (cmd == IDM_THEME_LIGHT || cmd == IDM_THEME_DARK) {
                              bool dark = (cmd == IDM_THEME_DARK);
                              if (dark != g_dark_mode) { g_dark_mode = dark; apply_theme(); }
                          }
                      }

                      // ---------------- main window layout ----------------
                      //
                      // Panes start flush at x = pad. The toolbar overlay hosts Options / Restore
                      // and is placed on top of the first docked pane's header row and raised in
                      // the sibling z-order. Detached panes are excluded from the width math.

                      static void main_layout(void) {
                          RECT r;
                          GetClientRect(g_main_window, &r);
                          const int pad      = D(8);
                          const int gap      = D(6);
                          const int h_row    = D(28);
                          const int banner_h = D(36);

                          if (g_banner) MoveWindow(g_banner, 0, 0, r.right, banner_h, TRUE);

                          int status_h = 0;
                          if (g_status) {
                              RECT sr; GetWindowRect(g_status, &sr);
                              status_h = sr.bottom - sr.top;
                          }

                          const int toolbar_y = banner_h + pad;
                          const int bw_opts   = D(96);
                          const int bw_rst    = D(84);
                          const int ovl_x     = pad + D(4);
                          const int ovl_w     = bw_opts + gap + bw_rst;
                          const int btns_w    = D(4) + ovl_w + D(8);

                          const int pane_x0 = pad;
                          const int pane_y  = toolbar_y;
                          int pane_h = r.bottom - pane_y - pad - status_h;
                          if (pane_h < D(60)) pane_h = D(60);

                          // Identify first docked pane BEFORE moving anything, so WM_SIZE
                          // dispatched by MoveWindow sees the right value.
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
                              // No pane to host the overlay — park it at the top-left corner.
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

                          // Position and raise the toolbar overlay on top of the first pane.
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
                                          m->ptMinTrackSize.x = D(640);
                                          m->ptMinTrackSize.y = D(400);
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
                                              {
                                                  wchar_t buf[32];
                                                  wsprintfW(buf, L"%.12s", ua->local_sha.c_str());
                                                  msg += buf;
                                              }
                                              msg += L"\n  Latest (main): ";
                                              {
                                                  wchar_t buf[32];
                                                  wsprintfW(buf, L"%.12s", ua->remote_sha.c_str());
                                                  msg += buf;
                                              }
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

                          g_font_ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

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

                          /* Toolbar overlay — holds the two buttons, sits above the first pane. */
                          const int bw_opts = D(96);
                          const int bw_rst  = D(84);
                          const int gap     = D(6);
                          const int h_row   = D(28);

                          g_toolbar_overlay = CreateWindowExW(
                              0, L"OLASToolbarOverlay", L"",
                              WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                              0, 0, 0, 0, g_main_window, nullptr, hInst, nullptr);

                          HWND opts_btn = CreateWindowExW(
                              0, L"BUTTON", L"Options \u25BE",
                              BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              0, 0, bw_opts, h_row, g_toolbar_overlay, (HMENU)ID_OPTIONS, hInst, nullptr);
                          SendMessageW(opts_btn, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

                          HWND rst = CreateWindowExW(
                              0, L"BUTTON", L"Restore",
                              BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE,
                              bw_opts + gap, 0, bw_rst, h_row, g_toolbar_overlay, (HMENU)ID_RESTORE, hInst, nullptr);
                          SendMessageW(rst, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

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
                                                    SetWindowTextW(p->toggle_btn, p->enabled ? L"\u23F9" : L"\u25B6");
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
                                                    if (g_window_brush){ DeleteObject(g_window_brush);g_window_brush= nullptr; }
                                                    if (g_banner_brush){ DeleteObject(g_banner_brush);g_banner_brush= nullptr; }
                                                }
