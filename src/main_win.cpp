// main_win.cpp — OLAS for Windows: entry point, CLI, wiring.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "moonshine_engine.h"
#include "transcript_sink.h"
#include "win32_ui.h"

extern "C" {
#include "capture.h"
}

using namespace olas;

// ---------------- config ----------------

static std::vector<LanguageConfig> g_configs;
static double g_silence_rms        = DEF_SILENCE_RMS;
static int    g_capture_chunk_ms   = DEFAULT_CAPTURE_CHUNK_MS;

static FILE *g_notes   = nullptr;
static std::atomic<bool> g_running{true};

// ---------------- Win32 transcript sink ----------------

class Win32Sink : public TranscriptSink {
public:
    void post(TranscriptEvent ev) override {
        win32_ui_post_update(ev.slot,
                             ev.prefix.c_str(),
                             ev.body.c_str(),
                             ev.is_final ? 1 : 0,
                             ev.is_error ? 1 : 0);
    }
};

// ---------------- capture thread ----------------

struct CaptureArgs {
    Engine *engine = nullptr;
    int     chunk_samples = 0;
};

static void capture_loop(CaptureArgs *a) {
    std::vector<int16_t> pcm((size_t)a->chunk_samples);
    while (g_running.load()) {
        if (!capture_read_chunk(pcm.data())) {
            // capture_read_chunk() returns 0 whenever capture is transiently
            // stopped (e.g. device switch).  Only exit if we're actually
            // shutting down.
            if (!g_running.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        a->engine->feed(pcm);
    }
}

// ---------------- CLI helpers ----------------

static std::vector<std::string> argv_to_utf8(int &argc_out) {
    int argc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> out;
    if (!wargv) { argc_out = 0; return out; }
    out.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s((size_t)(n > 0 ? n - 1 : 0), '\0');
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], n, nullptr, nullptr);
        out.push_back(std::move(s));
    }
    LocalFree(wargv);
    argc_out = argc;
    return out;
}

static const char *DEF_MONITOR_SRC = "auto";

static void usage(const char *prog) {
    std::fprintf(stderr,
        "olas_win — Open Local Audio Scribe for Windows\n\n"
        "Usage: %s [options]\n\n"
        "  -m, --model PATH[,PATH]    Model directory per language\n"
        "  -a, --arch N[,N]           Architecture 0-5 per language [1=Base]\n"
        "  -l, --language CODE[,CODE] Comma-separated language codes [en,es]\n"
        "  -r, --rms THRESHOLD        Silence RMS threshold [%.0f]\n"
        "  -q, --chunk-ms MS          Capture chunk in ms [%d]\n"
        "  -h, --help                 Show this help\n\n"
        "Architecture numbers:\n"
        "  0=Tiny  1=Base  2=TinyStreaming  3=BaseStreaming\n"
        "  4=SmallStreaming  5=MediumStreaming\n",
        prog, DEF_SILENCE_RMS, DEFAULT_CAPTURE_CHUNK_MS);
}

// ---------------- UI callbacks ----------------

static Engine *g_engine = nullptr;

static void on_toggle(int slot) {
    if (!g_engine) return;
    g_engine->toggle(slot);
    win32_ui_set_pane_enabled(slot, g_engine->is_enabled(slot) ? 1 : 0);
}

static void on_device(int device_index) {
    // Restart capture on the new device. Audio is dropped briefly.
    win32_ui_post_status("switching capture device...");
    capture_stop();
    if (!capture_start(device_index)) {
        win32_ui_post_status("device switch failed");
        return;
    }
    win32_ui_post_status("listening...");
}

// ---------------- main ----------------

int main() {
    int argc = 0;
    std::vector<std::string> argv = argv_to_utf8(argc);

    std::vector<std::string> languages, models;
    std::vector<int>         arches;

    for (int i = 1; i < argc; ++i) {
        const std::string &a = argv[i];
        auto need = [&](const char *name) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", name);
                return false;
            }
            return true;
        };
        if (a == "-m" || a == "--model") {
            if (!need("--model")) return 1;
            models = split_csv(argv[++i]);
        } else if (a == "-l" || a == "--language") {
            if (!need("--language")) return 1;
            languages = split_csv(argv[++i]);
        } else if (a == "-a" || a == "--arch") {
            if (!need("--arch")) return 1;
            arches.clear();
            for (auto &tok : split_csv(argv[++i])) {
                int v;
                if (!parse_int(tok.c_str(), v) || !valid_arch(v)) {
                    std::fprintf(stderr, "invalid --arch: %s\n", tok.c_str());
                    return 1;
                }
                arches.push_back(v);
            }
        } else if (a == "-r" || a == "--rms") {
            if (!need("--rms")) return 1;
            double v;
            if (!parse_double(argv[++i].c_str(), v) || v < 0.0) {
                std::fprintf(stderr, "invalid --rms\n"); return 1;
            }
            g_silence_rms = v;
        } else if (a == "-q" || a == "--chunk-ms") {
            if (!need("--chunk-ms")) return 1;
            int v;
            if (!parse_int(argv[++i].c_str(), v) || v < 20 || v > 1000) {
                std::fprintf(stderr, "invalid --chunk-ms\n"); return 1;
            }
            g_capture_chunk_ms = v;
        } else if (a == "-h" || a == "--help") {
            usage(argv[0].c_str()); return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            usage(argv[0].c_str()); return 1;
        }
    }

    if (languages.empty()) { languages.push_back("en"); languages.push_back("es"); }

    for (const auto &l : languages) {
        if (!is_supported_language(l)) {
            std::fprintf(stderr, "unsupported language: %s\n", l.c_str());
            return 1;
        }
    }
    if (languages.size() > 2) {
        std::fprintf(stderr, "olas_win supports at most two languages\n");
        return 1;
    }

    if (arches.empty()) arches.assign(languages.size(), ARCH_BASE);
    else if (arches.size() == 1 && languages.size() > 1)
        arches.assign(languages.size(), arches.front());
    else if (arches.size() != languages.size()) {
        std::fprintf(stderr, "--arch count mismatch\n"); return 1;
    }

    if (models.empty()) {
        for (const auto &l : languages) models.push_back("base-" + l);
    } else if (models.size() == 1 && languages.size() > 1) {
        models.assign(languages.size(), models.front());
    } else if (models.size() != languages.size()) {
        std::fprintf(stderr, "--model count mismatch\n"); return 1;
    }

    for (size_t i = 0; i < languages.size(); ++i) {
        LanguageConfig c;
        c.language   = languages[i];
        c.model_path = models[i];
        c.arch       = arches[i];
        g_configs.push_back(std::move(c));
    }

    // Notes file: next to the exe (matches Linux behaviour).
    g_notes = std::fopen(NOTES_FILE, "w");
    if (!g_notes) {
        MessageBoxW(nullptr, L"Could not create the notes file.",
                    L"OLAS", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Init capture (miniaudio / WASAPI).
    if (!capture_init()) {
        MessageBoxW(nullptr,
            L"Could not initialize audio (miniaudio / WASAPI).",
            L"OLAS", MB_OK | MB_ICONERROR);
        std::fclose(g_notes); return 1;
    }
    capture_set_chunk_samples(SAMPLE_RATE * g_capture_chunk_ms / 1000);

    // UI (owns the sink).
    Win32Sink sink;
    if (!win32_ui_init(languages)) {
        MessageBoxW(nullptr, L"Could not create the main window.",
                    L"OLAS", MB_OK | MB_ICONERROR);
        capture_uninit();
        std::fclose(g_notes); return 1;
    }
    win32_ui_populate_devices(0);

    // Engine.
    auto session_start = std::chrono::system_clock::now();
    Engine engine(g_configs, &sink, g_notes, g_silence_rms, session_start);
    g_engine = &engine;

    try {
        engine.start();
    } catch (const std::exception &e) {
        char msg[512];
        std::snprintf(msg, sizeof msg, "Failed to initialise Moonshine: %s", e.what());
        MessageBoxA(nullptr, msg, "OLAS", MB_OK | MB_ICONERROR);
        win32_ui_shutdown();
        capture_uninit();
        std::fclose(g_notes); return 1;
    }

    // Start capture.
    if (!capture_start(0)) {
        win32_ui_post_status("capture start failed");
    } else {
        win32_ui_post_status("listening...");
    }

    // Capture thread.
    CaptureArgs cargs;
    cargs.engine = &engine;
    cargs.chunk_samples = SAMPLE_RATE * g_capture_chunk_ms / 1000;
    std::thread cap_thread(capture_loop, &cargs);

    // Message loop (blocks until the main window closes).
    win32_ui_run(on_toggle, on_device);

    // Shutdown.
    // Shutdown.
    g_running.store(false);
    capture_stop();
    if (cap_thread.joinable()) cap_thread.join();

    engine.stop();          // stop() now flushes in-progress segments itself.
    g_engine = nullptr;

    win32_ui_shutdown();
    capture_uninit();

    if (g_notes) { std::fclose(g_notes); g_notes = nullptr; }

    return 0;
}
