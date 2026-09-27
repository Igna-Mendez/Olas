#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "moonshine-cpp.h"
#include "transcript_sink.h"

namespace olas {

// ---------- config (mirrors olas-gtk.cpp) ----------
constexpr int SAMPLE_RATE          = 16000;
constexpr int VAD_FRAME_MS         = 20;
constexpr int VAD_FRAME_SAMPLES    = SAMPLE_RATE / 1000 * VAD_FRAME_MS;
constexpr int VAD_HANGOVER_MS      = 600;
constexpr int VAD_PREROLL_MS       = 300;

constexpr int PARTIAL_INTERVAL_MS       = 500;
constexpr int PARTIAL_INTERVAL_SAMPLES  = SAMPLE_RATE / 1000 * PARTIAL_INTERVAL_MS;
constexpr int MIN_PARTIAL_AUDIO_MS      = 300;
constexpr int MAX_PARTIAL_WINDOW_MS     = 4000;
constexpr int MAX_PARTIAL_WINDOW_SAMPLES= SAMPLE_RATE / 1000 * MAX_PARTIAL_WINDOW_MS;

constexpr int MAX_SEGMENT_MS       = 8000;
constexpr int MAX_SEGMENT_SAMPLES  = SAMPLE_RATE / 1000 * MAX_SEGMENT_MS;
constexpr size_t MAX_FINAL_BACKLOG = 16;

constexpr int DEFAULT_CAPTURE_CHUNK_MS = 20;
constexpr size_t AUDIO_QUEUE_MAX_CHUNKS = 600;

constexpr int ARCH_TINY             = 0;
constexpr int ARCH_BASE             = 1;
constexpr int ARCH_TINY_STREAMING   = 2;
constexpr int ARCH_BASE_STREAMING   = 3;
constexpr int ARCH_SMALL_STREAMING  = 4;
constexpr int ARCH_MEDIUM_STREAMING = 5;

constexpr double DEF_SILENCE_RMS    = 100.0;
constexpr const char *NOTES_FILE    = "olas-moonshine-notes.txt";

struct LanguageConfig {
    std::string language;
    std::string model_path;
    int         arch = ARCH_BASE;
};

// ---------- small helpers ----------
bool is_streaming_arch(int a);
bool valid_arch(int a);
bool is_supported_language(const std::string &lang);
std::vector<std::string> split_csv(const std::string &s);
bool parse_int(const char *s, int &out);
bool parse_double(const char *s, double &out);

// ===========================================================================
//  Engine — owns one worker slot per language.
//
//  Thread-safe API: feed() may be called from the capture thread; toggle(),
//  is_enabled() and size() from the UI thread; start()/stop()/flush() from
//  the main thread.  All UI notifications go through the supplied
//  TranscriptSink (called on worker threads).
// ===========================================================================
class Engine {
public:
    Engine(const std::vector<LanguageConfig> &configs,
           TranscriptSink                     *sink,
           FILE                               *notes,
           double                              silence_rms,
           std::chrono::system_clock::time_point session_start);
    ~Engine();

    Engine(const Engine &)            = delete;
    Engine &operator=(const Engine &) = delete;

    void start();
    void stop();
    void flush();

    // Called from the capture thread.
    void feed(const std::vector<int16_t> &pcm);

    // Called from the UI thread.
    void toggle(int slot);
    bool is_enabled(int slot) const;
    size_t size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace olas