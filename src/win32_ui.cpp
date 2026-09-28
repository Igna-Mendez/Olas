// win32_ui.cpp — Win32 UI for OLAS (one pane per language).
//
// Layout:
//   banner:  [OLAS] title strip
//   toolbar: [Options v] [Restore]
//   options: slide-down panel (device, zoom, stamps, theme)
//   panes:   one per language, split 50/50 (or full width if only one)
//   status:  bottom status bar
//
// Each pane:
//   [LANG] (centered, bold)
//   [▶/⏹] [▾/▸] [Decouple]   (row under the label)
//   RichEdit (read-only, monospace, color tags, resizable font)
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

#define WM_APP_UPDATE (WM_APP + 1)
#define WM_APP_STATUS (WM_APP + 2)
#define WM_APP_UPDATE_AVAIL (WM_APP + 3)
// ---------------- IDs ----------------

enum {
    ID_TOGGLE    = 101,
    ID_DECOUPLE  = 102,
    ID_STAMPS    = 103,
    ID_ZOOM_IN   = 104,   /* kept for backward compat, no button */
    ID_ZOOM_OUT  = 105,   /* kept for backward compat, no button */
    ID_STATUS    = 106,
    ID_EDIT      = 107,
    ID_COLLAPSE  = 108,
    ID_OPTIONS   = 109,   /* toolbar toggle button */
    ID_RESTORE   = 110,
    ID_OPT_PANEL = 111,   /* the slide-down panel itself */
};

enum {
    ID_OPT_DEVICE  = 200,
    ID_OPT_ZOOM    = 201,
    ID_OPT_STAMPS  = 202,
    ID_OPT_THEME_L = 203,
    ID_OPT_THEME_D = 204,
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

static HWND        g_main_window    = nullptr;
static HWND        g_status         = nullptr;
static HWND        g_options_panel  = nullptr;
static bool        g_options_visible = false;
static HFONT       g_font_mono      = nullptr;
static HFONT       g_font_ui        = nullptr;
static HWND g_opt_device_combo = nullptr;
static HWND g_opt_zoom_track   = nullptr;
static HWND g_opt_stamps_chk   = nullptr;
static HWND g_opt_light_radio  = nullptr;
static HWND g_opt_dark_radio   = nullptr;

static std::vector<std::unique_ptr<Pane>> g_panes;
static int         g_body_pt       = 14;
static bool        g_show_stamps   = true;
static UINT        g_dpi           = 96;

static Pane *g_floated_pane   = nullptr;
static Pane *g_collapsed_pane = nullptr;

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
static void create_options_panel(HINSTANCE hInst);
static void toggle_options_panel(void);

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

                      // ---------------- pane layout ----------------

                      static void pane_layout(Pane *p) {
                          if (!p || !p->container) return;
                          RECT r;
                          GetClientRect(p->container, &r);
                          const int pad      = D(4);
                          const int label_h  = D(24);
                          const int button_h = D(28);
                          const int hdr      = label_h + button_h;

                          MoveWindow(p->label, 0, 0, r.right, label_h, TRUE);

                          int bw = D(84), bh = D(22);
                          int x = r.right - pad - bw;
                          MoveWindow(p->detach_btn,   x, label_h + D(3), bw, bh, TRUE);
                          x -= D(40) + D(4);
                          MoveWindow(p->collapse_btn, x, label_h + D(3), D(40), bh, TRUE);
                          x -= D(40) + D(4);
                          MoveWindow(p->toggle_btn,   x, label_h + D(3), D(40), bh, TRUE);

                          MoveWindow(p->edit, pad, hdr,
                                     r.right - 2 * pad, r.bottom - hdr - pad, TRUE);
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
                      }

                      static void pane_reattach(Pane *p) {
                          if (!p || !p->float_window) return;
                          HWND w = p->float_window;
                          p->float_window = nullptr;
                          g_floated_pane  = nullptr;

                          SetParent(p->container, g_main_window);
                          ShowWindow(p->container, SW_SHOW);
                          DestroyWindow(w);

                          SetWindowTextW(p->detach_btn, L"Decouple");
                          for (auto &pp : g_panes) EnableWindow(pp->detach_btn, TRUE);

                          RECT r; GetClientRect(g_main_window, &r);
                          SendMessageW(g_main_window, WM_SIZE, 0, MAKELPARAM(r.right, r.bottom));
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
                      }

                      // ---------------- pane creation ----------------

                      static void pane_create_controls(Pane *p, HINSTANCE hInst) {
                          p->container = CreateWindowExW(
                              WS_EX_CONTROLPARENT, L"OLASPane", L"",
                              WS_CHILD | WS_VISIBLE,
                              0, 0, 0, 0, g_main_window, nullptr, hInst, nullptr);
                          SetWindowLongPtrW(p->container, GWLP_USERDATA, (LONG_PTR)p);

                          p->label = CreateWindowExW(0, L"STATIC", L"",
                                                     WS_CHILD | WS_VISIBLE | SS_CENTER,
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
                                      LONG interArea = (inter.right - inter.left) *
                                      (inter.bottom - inter.top);
                                      LONG floatArea = (floatR.right - floatR.left) *
                                      (floatR.bottom - floatR.top);
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

                          if (g_opt_zoom_track) {
                              int p = (int)SendMessageW(g_opt_zoom_track, TBM_GETPOS, 0, 0);
                              if (p != g_body_pt)
                                  SendMessageW(g_opt_zoom_track, TBM_SETPOS, TRUE, g_body_pt);
                          }

                          for (auto &pp : g_panes) {
                              if (pp->edit)
                                  SendMessageW(pp->edit, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
                              pane_rerender(pp.get());
                          }
                      }

                      static void apply_theme(void) {
                          g_theme = g_dark_mode ? THEME_DARK : THEME_LIGHT;

                          /* Create new brushes before swapping so no handler ever sees a stale
                           * brush handle. */
                          HBRUSH new_window = CreateSolidBrush(g_theme.window_bg);
                          HBRUSH new_banner = CreateSolidBrush(g_theme.banner_bg);

                          HBRUSH old_window = g_window_brush;
                          HBRUSH old_banner = g_banner_brush;
                          g_window_brush = new_window;
                          g_banner_brush = new_banner;

                          /* Update RichEdit backgrounds and rebuild text with the new text/stamp
                           * colors. */
                          for (auto &pp : g_panes) {
                              if (!pp->edit) continue;
                              SendMessageW(pp->edit, EM_SETBKGNDCOLOR, 0, (LPARAM)g_theme.bg);
                              pane_rerender(pp.get());
                          }

                          /* Win11 caption tint (no-op on Win10). */
                          if (g_main_window) {
                              COLORREF cap_bg   = g_theme.banner_bg;
                              COLORREF cap_text = RGB(0xFF, 0xFF, 0xFF);
                              DwmSetWindowAttribute(g_main_window, DWMWA_CAPTION_COLOR,
                                                    &cap_bg, sizeof(cap_bg));
                              DwmSetWindowAttribute(g_main_window, DWMWA_TEXT_COLOR,
                                                    &cap_text, sizeof(cap_text));
                          }

                          /* Sledgehammer: synchronously repaint the top-level window and every
                           * descendant (panes, labels, buttons, options panel, status bar). This
                           * is what actually makes WM_ERASEBKGND / WM_CTLCOLOR* fire on the
                           * children — a plain InvalidateRect(g_main_window) does not reach
                           * them, which was why dark mode looked like a no-op before. */
                          if (g_main_window) {
                              RedrawWindow(g_main_window, nullptr, nullptr,
                                           RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
                          }
                          if (g_banner) {
                              RedrawWindow(g_banner, nullptr, nullptr,
                                           RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
                          }

                          if (old_window) DeleteObject(old_window);
                          if (old_banner) DeleteObject(old_banner);
                      }

                      // ---------------- options panel (slide-down) ----------------

                        static LRESULT CALLBACK options_panel_proc(HWND hwnd, UINT msg,
                                                                 WPARAM wp, LPARAM lp) {
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

                              case WM_COMMAND: {
                                  switch (LOWORD(wp)) {
                                      case ID_OPT_DEVICE:
                                          if (HIWORD(wp) == CBN_SELCHANGE && g_on_device) {
                                              int idx = (int)SendMessageW(g_opt_device_combo,
                                                                          CB_GETCURSEL, 0, 0);
                                              if (idx != CB_ERR) {
                                                  g_current_device_index = idx;
                                                  g_on_device(idx);
                                              }
                                          }
                                          return 0;

                                      case ID_OPT_STAMPS: {
                                          bool stamps = SendMessageW(g_opt_stamps_chk,
                                                                     BM_GETCHECK, 0, 0) == BST_CHECKED;
                                                                     if (stamps != g_show_stamps) {
                                                                         g_show_stamps = stamps;
                                                                         for (auto &pp : g_panes) pane_rerender(pp.get());
                                                                     }
                                                                     return 0;
                                      }

                                      case ID_OPT_THEME_L:
                                      case ID_OPT_THEME_D: {
                                          bool dark = SendMessageW(g_opt_dark_radio,
                                                                   BM_GETCHECK, 0, 0) == BST_CHECKED;
                                                                   if (dark != g_dark_mode) {
                                                                       g_dark_mode = dark;
                                                                       apply_theme();
                                                                   }
                                                                   return 0;
                                      }
                                  }
                                  return 0;
                              }

                                      case WM_HSCROLL: {
                                          HWND ctl = (HWND)lp;
                                          if (ctl == g_opt_zoom_track) {
                                              int pos = (int)SendMessageW(g_opt_zoom_track,
                                                                          TBM_GETPOS, 0, 0);
                                              if (pos != g_body_pt) apply_zoom_to(pos);
                                          }
                                          return 0;
                                      }
                          }
                          return DefWindowProcW(hwnd, msg, wp, lp);
                                                                 }

                                                                 static void create_options_panel(HINSTANCE hInst) {
                                                                     /* Child of the main window, hidden until the toolbar toggle shows it.
                                                                      * WS_CLIPCHILDREN prevents flicker on resize. */
                                                                     g_options_panel = CreateWindowExW(
                                                                         0, L"OLASOptionsPanel", L"",
                                                                         WS_CHILD | WS_CLIPCHILDREN,
                                                                         0, 0, 0, 0, g_main_window, (HMENU)ID_OPT_PANEL, hInst, nullptr);
                                                                     if (!g_options_panel) return;

                                                                     const int pad = D(12);
                                                                     int y = D(10);

                                                                     /* Row 1: device */
                                                                     CreateWindowExW(0, L"STATIC", L"Capture device:",
                                                                                     WS_CHILD | WS_VISIBLE, pad, y + D(3), D(110), D(20),
                                                                                     g_options_panel, nullptr, hInst, nullptr);

                                                                     g_opt_device_combo = CreateWindowExW(0, L"COMBOBOX", L"",
                                                                                                          CBS_DROPDOWNLIST | WS_VSCROLL | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                                                                          pad + D(120), y, D(300), D(200),
                                                                                                          g_options_panel, (HMENU)ID_OPT_DEVICE, hInst, nullptr);
                                                                     SendMessageW(g_opt_device_combo, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
                                                                     y += D(32);

                                                                     /* Row 2: zoom */
                                                                     CreateWindowExW(0, L"STATIC", L"Zoom:",
                                                                                     WS_CHILD | WS_VISIBLE, pad, y + D(4), D(60), D(20),
                                                                                     g_options_panel, nullptr, hInst, nullptr);

                                                                     g_opt_zoom_track = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                                                                                        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_HORZ,
                                                                                                        pad + D(70), y, D(360), D(28),
                                                                                                        g_options_panel, (HMENU)ID_OPT_ZOOM, hInst, nullptr);
                                                                     SendMessageW(g_opt_zoom_track, TBM_SETRANGE, TRUE, MAKELONG(8, 32));
                                                                     SendMessageW(g_opt_zoom_track, TBM_SETPOS,   TRUE, g_body_pt);
                                                                     y += D(40);

                                                                     /* Row 3: stamps + theme */
                                                                     g_opt_stamps_chk = CreateWindowExW(0, L"BUTTON", L"Show timestamps",
                                                                                                        BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                                                                        pad, y, D(180), D(22),
                                                                                                        g_options_panel, (HMENU)ID_OPT_STAMPS, hInst, nullptr);
                                                                     SendMessageW(g_opt_stamps_chk, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
                                                                     SendMessageW(g_opt_stamps_chk, BM_SETCHECK,
                                                                                  g_show_stamps ? BST_CHECKED : BST_UNCHECKED, 0);

                                                                     CreateWindowExW(0, L"STATIC", L"Appearance:",
                                                                                     WS_CHILD | WS_VISIBLE, pad + D(210), y + D(3), D(80), D(20),
                                                                                     g_options_panel, nullptr, hInst, nullptr);

                                                                     g_opt_light_radio = CreateWindowExW(0, L"BUTTON", L"Light",
                                                                                                         BS_AUTORADIOBUTTON | WS_CHILD | WS_VISIBLE | WS_GROUP | WS_TABSTOP,
                                                                                                         pad + D(300), y, D(60), D(22),
                                                                                                         g_options_panel, (HMENU)ID_OPT_THEME_L, hInst, nullptr);
                                                                     g_opt_dark_radio = CreateWindowExW(0, L"BUTTON", L"Dark",
                                                                                                        BS_AUTORADIOBUTTON | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                                                                        pad + D(370), y, D(60), D(22),
                                                                                                        g_options_panel, (HMENU)ID_OPT_THEME_D, hInst, nullptr);
                                                                     SendMessageW(g_opt_light_radio, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
                                                                     SendMessageW(g_opt_dark_radio,  WM_SETFONT, (WPARAM)g_font_ui, TRUE);
                                                                     SendMessageW(g_opt_light_radio, BM_SETCHECK, !g_dark_mode, 0);
                                                                     SendMessageW(g_opt_dark_radio,  BM_SETCHECK,  g_dark_mode, 0);
                                                                 }

                                                                 static void toggle_options_panel(void) {
                                                                     g_options_visible = !g_options_visible;
                                                                     if (g_options_panel)
                                                                         ShowWindow(g_options_panel, g_options_visible ? SW_SHOW : SW_HIDE);

                                                                     HWND btn = GetDlgItem(g_main_window, ID_OPTIONS);
                                                                     if (btn)
                                                                         SetWindowTextW(btn, g_options_visible ? L"Options \u25B4"
                                                                         : L"Options \u25BE");
                                                                     main_layout();
                                                                 }

                                                                 // ---------------- main window layout ----------------

                                                                 static void main_layout(void) {
                                                                     RECT r;
                                                                     GetClientRect(g_main_window, &r);
                                                                     const int pad = D(8), h_row = D(28);
                                                                     const int banner_h = D(36);

                                                                     if (g_banner) MoveWindow(g_banner, 0, 0, r.right, banner_h, TRUE);

                                                                     int status_h = 0;
                                                                     if (g_status) {
                                                                         RECT sr; GetWindowRect(g_status, &sr);
                                                                         status_h = sr.bottom - sr.top;
                                                                     }

                                                                     /* Toolbar: [Options v] [Restore] */
                                                                     int x = pad, y = banner_h + pad;
                                                                     const int gap = D(4);

                                                                     MoveWindow(GetDlgItem(g_main_window, ID_OPTIONS), x, y, D(96), h_row, TRUE);
                                                                     x += D(96) + gap;
                                                                     MoveWindow(GetDlgItem(g_main_window, ID_RESTORE), x, y, D(84), h_row, TRUE);

                                                                     int content_y = y + h_row + pad;

                                                                     /* Options slide-down panel */
                                                                     const int panel_h = D(130);
                                                                     if (g_options_panel && g_options_visible) {
                                                                         MoveWindow(g_options_panel, pad, content_y,
                                                                                    r.right - 2 * pad, panel_h, TRUE);
                                                                         content_y += panel_h + pad;
                                                                     }

                                                                     /* Panes */
                                                                     int pane_y = content_y;
                                                                     int pane_h = r.bottom - pane_y - pad - status_h;
                                                                     if (pane_h < D(60)) pane_h = D(60);

                                                                     int n = (int)g_panes.size();
                                                                     if (n == 0) return;
                                                                     int total_w = r.right - 2 * pad - (n - 1) * gap;

                                                                     int collapsed_w = D(160);
                                                                     bool any_collapsed = (g_collapsed_pane != nullptr) && (n > 1);

                                                                     int px = pad;
                                                                     for (int i = 0; i < n; ++i) {
                                                                         Pane *p = g_panes[i].get();
                                                                         if (!p->container) continue;
                                                                         int w;
                                                                         if (any_collapsed) {
                                                                             w = (p == g_collapsed_pane) ? collapsed_w
                                                                             : (total_w - collapsed_w);
                                                                         } else {
                                                                             w = total_w / n;
                                                                         }
                                                                         if (!p->float_window)
                                                                             MoveWindow(p->container, px, pane_y, w, pane_h, TRUE);
                                                                         px += w + gap;
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
                                                                                 case ID_STAMPS:
                                                                                     g_show_stamps = !g_show_stamps;
                                                                                     if (g_opt_stamps_chk)
                                                                                         SendMessageW(g_opt_stamps_chk, BM_SETCHECK,
                                                                                                      g_show_stamps ? BST_CHECKED
                                                                                                      : BST_UNCHECKED, 0);
                                                                                         for (auto &pp : g_panes) pane_rerender(pp.get());
                                                                                         return 0;
                                                                                 case ID_OPTIONS:
                                                                                     toggle_options_panel();
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

                                                                     wc.lpfnWndProc   = options_panel_proc;
                                                                     wc.hbrBackground = nullptr;
                                                                     wc.lpszClassName = L"OLASOptionsPanel";
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
                                                                         DwmSetWindowAttribute(g_main_window, DWMWA_CAPTION_COLOR,
                                                                                               &cap_bg, sizeof(cap_bg));
                                                                         DwmSetWindowAttribute(g_main_window, DWMWA_TEXT_COLOR,
                                                                                               &cap_text, sizeof(cap_text));
                                                                     }

                                                                     /* Banner */
                                                                     g_banner = CreateWindowExW(0, L"OLASBanner", L"",
                                                                                                WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                                                                                g_main_window, nullptr, hInst, nullptr);

                                                                     /* Toolbar */
                                                                     HWND opts_btn = CreateWindowExW(0, L"BUTTON", L"Options \u25BE",
                                                                                                     BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                                                                     0, 0, 0, 0, g_main_window, (HMENU)ID_OPTIONS, hInst, nullptr);
                                                                     SendMessageW(opts_btn, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

                                                                     HWND rst = CreateWindowExW(0, L"BUTTON", L"Restore",
                                                                                                BS_PUSHBUTTON | WS_CHILD | WS_VISIBLE,
                                                                                                0, 0, 0, 0, g_main_window, (HMENU)ID_RESTORE, hInst, nullptr);
                                                                     SendMessageW(rst, WM_SETFONT, (WPARAM)g_font_ui, TRUE);

                                                                     /* Options slide-down panel (hidden by default). */
                                                                     create_options_panel(hInst);

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
                                                                     return 1;
                                                                 }

                                                                 void win32_ui_populate_devices(int default_index) {
                                                                     if (!g_opt_device_combo) return;
                                                                     int n = capture_device_count();
                                                                     for (int i = 0; i < n; ++i) {
                                                                         const char *name = capture_device_name(i);
                                                                         if (!name || !*name) continue;
                                                                         wchar_t *w = a2w(name);
                                                                         if (!w) continue;
                                                                         int idx = (int)SendMessageW(g_opt_device_combo, CB_ADDSTRING, 0, (LPARAM)w);
                                                                         SendMessageW(g_opt_device_combo, CB_SETITEMDATA, idx, (LPARAM)i);
                                                                         free(w);
                                                                     }
                                                                     if (default_index >= 0 && default_index < n) {
                                                                         SendMessageW(g_opt_device_combo, CB_SETCURSEL, default_index, 0);
                                                                         g_current_device_index = default_index;
                                                                     } else if (n > 0) {
                                                                         SendMessageW(g_opt_device_combo, CB_SETCURSEL, 0, 0);
                                                                         g_current_device_index = 0;
                                                                     }
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
                                                                                               g_options_panel = nullptr;
                                                                                               g_panes.clear();
                                                                                               if (g_font_mono)   { DeleteObject(g_font_mono);   g_font_mono   = nullptr; }
                                                                                               if (g_font_label)  { DeleteObject(g_font_label);  g_font_label  = nullptr; }
                                                                                               if (g_font_glyph)  { DeleteObject(g_font_glyph);  g_font_glyph  = nullptr; }
                                                                                               if (g_font_banner) { DeleteObject(g_font_banner); g_font_banner = nullptr; }
                                                                                               if (g_window_brush){ DeleteObject(g_window_brush);g_window_brush= nullptr; }
                                                                                               if (g_banner_brush){ DeleteObject(g_banner_brush);g_banner_brush= nullptr; }
                                                                                           }
