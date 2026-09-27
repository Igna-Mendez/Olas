#pragma once
#include <string>
#include <vector>

// Win32 UI for OLAS.  Owns one "pane" per language, each pane containing a
// header (language label, Start/Stop, Detach) and a read-only RichEdit
// transcript.  Panes can be detached into standalone top-level windows.

typedef void (*olas_toggle_fn)(int slot);
typedef void (*olas_device_fn)(int device_index);

// Build the main window with one pane per language (max 2). Returns 1 on
// success, 0 on failure. Call before win32_ui_run.
int  win32_ui_init(const std::vector<std::string> &languages);

// Populate the device combo using capture_device_count()/capture_device_name()
// from capture.h. Call after win32_ui_init.
void win32_ui_populate_devices(int default_index);

// Run the message loop (blocks until the main window closes). Registers the
// toggle/device callbacks. The UI will invoke them on the UI thread.
void win32_ui_run(olas_toggle_fn on_toggle, olas_device_fn on_device);

// Thread-safe: post a transcript update for pane `slot`.
void win32_ui_post_update(int slot, const char *prefix, const char *body,
                          int is_final, int is_error);

// Thread-safe: post a status-bar message.
void win32_ui_post_status(const char *text);

// UI-thread: update the Start/Stop button label of a pane.
void win32_ui_set_pane_enabled(int slot, int enabled);

// UI-thread: destroy the window and free resources.
void win32_ui_shutdown();