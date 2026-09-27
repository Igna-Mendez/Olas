#include "moonshine_engine.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace olas {

// ---------- helpers ----------

std::vector<std::string> split_csv(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

bool parse_int(const char *s, int &out) {
    if (!s || !*s) return false;
    char *end = nullptr;
    errno = 0;
    long v = std::strtol(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0' ||
        v < std::numeric_limits<int>::min() ||
        v > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(v);
    return true;
}

bool parse_double(const char *s, double &out) {
    if (!s || !*s) return false;
    char *end = nullptr;
    errno = 0;
    double v = std::strtod(s, &end);
    if (errno == ERANGE || end == s || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool is_streaming_arch(int a) {
    return a >= ARCH_TINY_STREAMING && a <= ARCH_MEDIUM_STREAMING;
}
bool valid_arch(int a) { return a >= ARCH_TINY && a <= ARCH_MEDIUM_STREAMING; }

bool is_supported_language(const std::string &lang) {
#ifdef OLAS_MOONSHINE_EXTENDED_LANGUAGES
    static const std::vector<std::string> s = {
        "en","es","ar","ja","ko","zh","vi","uk"
    };
#else
    static const std::vector<std::string> s = { "en", "es" };
#endif
    for (const auto &x : s) if (x == lang) return true;
    return false;
}

static double frame_rms(const int16_t *p, int n) {
    if (!p || n <= 0) return 0.0;
    double acc = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = static_cast<double>(p[i]);
        acc += x * x;
    }
    return std::sqrt(acc / static_cast<double>(n));
}

// ---------- AudioQueue ----------

class AudioQueue {
public:
    explicit AudioQueue(size_t max_chunks) : max_chunks_(max_chunks) {}

    bool push(std::vector<int16_t> &&chunk) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (closed_) return false;
        if (queue_.size() >= max_chunks_) {
            queue_.pop_front();
        }
        queue_.push_back(std::move(chunk));
        cv_.notify_one();
        return true;
    }

    bool pop(std::vector<int16_t> &out) {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lk(mutex_);
        closed_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<int16_t>> queue_;
    size_t max_chunks_;
    bool closed_ = false;
};

// ---------- StatefulVad ----------

class StatefulVad {
public:
    struct Segment { std::vector<int16_t> audio; uint64_t start_sample = 0; };

    StatefulVad(double rms_threshold, int hangover_ms, int preroll_ms,
                int max_segment_ms)
        : rms_threshold_(rms_threshold),
          hangover_frames_(std::max(1, hangover_ms / VAD_FRAME_MS)),
          preroll_frames_(std::max(0, preroll_ms / VAD_FRAME_MS)),
          max_segment_samples_(SAMPLE_RATE / 1000 * max_segment_ms) {}

    void reset() {
        state_ = State::Silence; silence_count_ = 0;
        preroll_.clear(); segment_.clear();
        preroll_start_sample_ = 0; segment_start_sample_ = 0;
    }

    bool feed(const int16_t *frame, int n, uint64_t frame_start_sample,
              Segment &out) {
        if (!frame || n != VAD_FRAME_SAMPLES) return false;
        const double r = frame_rms(frame, n);

        if (state_ == State::Silence) {
            preroll_.emplace_back(frame, frame + n);
            preroll_start_sample_ = frame_start_sample;
            while (static_cast<int>(preroll_.size()) > preroll_frames_) {
                preroll_.pop_front();
                preroll_start_sample_ += VAD_FRAME_SAMPLES;
            }
            if (r >= rms_threshold_) {
                state_ = State::Speech; silence_count_ = 0; segment_.clear();
                segment_start_sample_ =
                    preroll_.empty() ? frame_start_sample : preroll_start_sample_;
                if (preroll_.empty())
                    segment_.insert(segment_.end(), frame, frame + n);
                else
                    for (const auto &f : preroll_)
                        segment_.insert(segment_.end(), f.begin(), f.end());
                preroll_.clear();
            }
            return false;
        }

        segment_.insert(segment_.end(), frame, frame + n);

        if (max_segment_samples_ > 0) {
            const uint64_t elapsed =
                (frame_start_sample + (uint64_t)n) - segment_start_sample_;
            if (elapsed >= (uint64_t)max_segment_samples_) {
                out.audio = std::move(segment_);
                out.start_sample = segment_start_sample_;
                segment_.clear(); preroll_.clear();
                state_ = State::Silence; silence_count_ = 0;
                return true;
            }
        }

        if (r < rms_threshold_) {
            ++silence_count_;
            if (silence_count_ >= hangover_frames_) {
                out.audio = std::move(segment_);
                out.start_sample = segment_start_sample_;
                segment_.clear(); preroll_.clear();
                state_ = State::Silence; silence_count_ = 0;
                return true;
            }
        } else silence_count_ = 0;
        return false;
    }

    bool flush(Segment &out) {
        if (state_ != State::Speech || segment_.empty()) return false;
        out.audio = std::move(segment_);
        out.start_sample = segment_start_sample_;
        segment_.clear(); preroll_.clear();
        state_ = State::Silence; silence_count_ = 0;
        return true;
    }

    bool in_speech() const { return state_ == State::Speech; }
    const std::vector<int16_t> &current_audio() const { return segment_; }
    uint64_t current_start_sample() const { return segment_start_sample_; }

private:
    enum class State { Silence, Speech };
    double rms_threshold_;
    int hangover_frames_, preroll_frames_;
    int max_segment_samples_ = 0;
    int silence_count_ = 0;
    State state_ = State::Silence;
    uint64_t preroll_start_sample_ = 0, segment_start_sample_ = 0;
    std::deque<std::vector<int16_t>> preroll_;
    std::vector<int16_t> segment_;
};

// ---------- PrintListener ----------

class PrintListener : public moonshine::TranscriptEventListener {
public:
    PrintListener(FILE *notes, TranscriptSink *sink, std::string language,
                  int slot_idx, std::chrono::system_clock::time_point session_start)
        : notes_(notes), sink_(sink), language_(std::move(language)),
          slot_idx_(slot_idx), session_start_(session_start) {}

    void set_stream_start(std::chrono::system_clock::time_point tp) {
        session_start_ = tp;
    }
    void set_segment_start(double rel_time) { current_start_ = rel_time; }
    void add_paused_ms(int64_t ms) { paused_ms_.fetch_add(ms); }

    void emit_partial(const std::string &text) {
        if (text.empty() || !sink_) return;
        auto [pre, body] = split(text, current_start_);
        sink_->post({ slot_idx_, std::move(pre), std::move(body), false, false });
    }

    void emit_final(const std::string &text, double rel_time) {
        if (text.empty() || !sink_) return;
        auto [pre, body] = split(text, rel_time);

        if (notes_) {
            std::string full = pre + body + "\n";
            std::fwrite(full.data(), 1, full.size(), notes_);
            std::fflush(notes_);
        }
        sink_->post({ slot_idx_, std::move(pre), std::move(body), true, false });
    }

    void onLineTextChanged(const moonshine::LineTextChanged &e) override {
        emit_partial(e.line.text);
    }
    void onLineCompleted(const moonshine::LineCompleted &e) override {
        emit_final(e.line.text, e.line.startTime);
    }
    void onError(const moonshine::Error &e) override {
        if (sink_)
            sink_->post({ slot_idx_, {}, e.errorMessage, true, true });
    }

private:
    std::pair<std::string, std::string> split(const std::string &text, double rel) {
        double sec = rel;
        if (!std::isfinite(sec) || sec < 0.0) sec = 0.0;
        sec += static_cast<double>(paused_ms_.load()) / 1000.0;
        auto tp = session_start_ +
                  std::chrono::milliseconds((long long)(sec * 1000.0));
        std::time_t tt = std::chrono::system_clock::to_time_t(tp);
        struct tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &tt);
#else
        localtime_r(&tt, &tmv);
#endif
        char wall[16];
        std::strftime(wall, sizeof wall, "%H:%M:%S", &tmv);

        std::string pre;
        pre.reserve(32);
        pre += "["; pre += wall; pre += "] [";
        pre += language_; pre += "] ";
        return { std::move(pre), text };
    }

    FILE                                *notes_;
    TranscriptSink                      *sink_;
    std::string                          language_;
    int                                  slot_idx_;
    std::chrono::system_clock::time_point session_start_;
    double                               current_start_ = 0.0;
    std::atomic<int64_t>                 paused_ms_{0};
};

// ---------- StreamingWorker ----------

class StreamingWorker {
public:
    StreamingWorker(const std::string &model_path, int arch,
                    std::chrono::system_clock::time_point session_start,
                    FILE *notes, TranscriptSink *sink,
                    const std::string &language, int slot_idx)
        : listener_(std::make_unique<PrintListener>(
              notes, sink, language, slot_idx, session_start)) {
        transcriber_ = std::make_unique<moonshine::Transcriber>(
            model_path, to_model_arch(arch), 0.25);
        transcriber_->addListener(listener_.get());
        transcriber_->start();
    }
    ~StreamingWorker() {
        if (transcriber_) { try { transcriber_->stop(); } catch (...) {} }
    }

    void set_session_start(std::chrono::system_clock::time_point tp) {
        listener_->set_stream_start(tp);
    }
    void add_paused_ms(int64_t ms) { listener_->add_paused_ms(ms); }

    void feed(const std::vector<int16_t> &pcm) {
        if (!transcriber_ || pcm.empty()) return;
        fb_.resize(pcm.size());
        for (size_t i = 0; i < pcm.size(); ++i)
            fb_[i] = static_cast<float>(pcm[i]) / 32768.0f;
        transcriber_->addAudio(fb_, SAMPLE_RATE);
    }
    void flush() {
        if (!transcriber_) return;
        try { transcriber_->updateTranscription(); }
        catch (const moonshine::MoonshineException &) {}
    }
    void reset() {}   // streaming worker has no VAD state

private:
    static moonshine::ModelArch to_model_arch(int a) {
        switch (a) {
            case ARCH_TINY:             return moonshine::ModelArch::TINY;
            case ARCH_BASE:             return moonshine::ModelArch::BASE;
            case ARCH_TINY_STREAMING:   return moonshine::ModelArch::TINY_STREAMING;
            case ARCH_BASE_STREAMING:   return moonshine::ModelArch::BASE_STREAMING;
            case ARCH_SMALL_STREAMING:  return moonshine::ModelArch::SMALL_STREAMING;
            case ARCH_MEDIUM_STREAMING: return moonshine::ModelArch::MEDIUM_STREAMING;
            default: throw std::invalid_argument("bad arch");
        }
    }
    std::unique_ptr<moonshine::Transcriber> transcriber_;
    std::unique_ptr<PrintListener>          listener_;
    std::vector<float>                      fb_;
};

// ---------- NonStreamingWorker ----------

class NonStreamingWorker {
public:
    NonStreamingWorker(const std::string &model_path, int arch,
                       std::chrono::system_clock::time_point session_start,
                       FILE *notes, TranscriptSink *sink,
                       double silence_rms,
                       const std::string &language, int slot_idx)
        : listener_(std::make_unique<PrintListener>(
              notes, sink, language, slot_idx, session_start)),
          vad_(std::make_unique<StatefulVad>(
              silence_rms, VAD_HANGOVER_MS, VAD_PREROLL_MS, MAX_SEGMENT_MS)) {
        const moonshine::ModelArch ma =
            (arch == ARCH_TINY) ? moonshine::ModelArch::TINY
                                : moonshine::ModelArch::BASE;
        transcriber_ = std::make_unique<moonshine::Transcriber>(
            model_path, ma, 0.25);
        inference_ = std::make_unique<InferenceThread>(
            transcriber_.get(), listener_.get());
    }

    void set_session_start(std::chrono::system_clock::time_point tp) {
        listener_->set_stream_start(tp);
    }
    void reset() {
        vad_->reset();
        pcm_rem_.clear();
        last_partial_at_ = 0;
        inference_->clear_pending_partial();
    }
    void add_paused_ms(int64_t ms) { listener_->add_paused_ms(ms); }

    void feed(const std::vector<int16_t> &pcm) {
        if (pcm.empty()) return;
        pcm_rem_.insert(pcm_rem_.end(), pcm.begin(), pcm.end());
        while (pcm_rem_.size() >= VAD_FRAME_SAMPLES) {
            const uint64_t frame_start = total_seen_;
            std::vector<int16_t> frame(pcm_rem_.begin(),
                                       pcm_rem_.begin() + VAD_FRAME_SAMPLES);
            pcm_rem_.erase(pcm_rem_.begin(),
                           pcm_rem_.begin() + VAD_FRAME_SAMPLES);
            total_seen_ += VAD_FRAME_SAMPLES;

            StatefulVad::Segment seg;
            if (vad_->feed(frame.data(), VAD_FRAME_SAMPLES, frame_start, seg)) {
                inference_->post_final(std::move(seg.audio), seg.start_sample);
                last_partial_at_ = 0;
                continue;
            }
            if (vad_->in_speech()) {
                const auto &audio = vad_->current_audio();
                const uint64_t start = vad_->current_start_sample();
                const uint64_t elapsed = total_seen_ - start;
                const uint64_t since   = total_seen_ - last_partial_at_;
                const bool enough = elapsed >=
                    (uint64_t)(SAMPLE_RATE * MIN_PARTIAL_AUDIO_MS / 1000);
                const bool due = last_partial_at_ == 0 ||
                    since >= (uint64_t)PARTIAL_INTERVAL_SAMPLES;
                if (enough && due) {
                    const size_t win = std::min(audio.size(),
                        (size_t)MAX_PARTIAL_WINDOW_SAMPLES);
                    const uint64_t ws = start + (audio.size() - win);
                    std::vector<int16_t> w(audio.end() - win, audio.end());
                    inference_->post_partial(std::move(w), ws);
                    last_partial_at_ = total_seen_;
                }
            }
        }
    }

    void flush() {
        StatefulVad::Segment seg;
        if (vad_->flush(seg))
            inference_->post_final(std::move(seg.audio), seg.start_sample);
        inference_->drain();
    }

private:
    class InferenceThread {
    public:
        InferenceThread(moonshine::Transcriber *t, PrintListener *l)
            : t_(t), l_(l) { th_ = std::thread([this] { run(); }); }
        ~InferenceThread() {
            { std::lock_guard<std::mutex> lk(m_); closing_ = true; cv_.notify_all(); }
            if (th_.joinable()) th_.join();
        }

        void post_partial(std::vector<int16_t> &&a, uint64_t s) {
            std::lock_guard<std::mutex> lk(m_);
            pending_partial_ = Work{std::move(a), s};
            cv_.notify_one();
        }
        void post_final(std::vector<int16_t> &&a, uint64_t s) {
            std::lock_guard<std::mutex> lk(m_);
            if (finals_.size() >= MAX_FINAL_BACKLOG) {
                finals_.pop_front();
                --pending_finals_;
            }
            finals_.push_back(Work{std::move(a), s});
            ++pending_finals_;
            cv_.notify_one();
        }
        void clear_pending_partial() {
            std::lock_guard<std::mutex> lk(m_);
            pending_partial_.reset();
        }
        void drain() {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return pending_finals_ == 0; });
        }

    private:
        struct Work { std::vector<int16_t> audio; uint64_t start_sample; };

        void run() {
            std::vector<float> fb;
            for (;;) {
                Work w; bool is_final;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [this] {
                        return closing_ || !finals_.empty() ||
                               pending_partial_.has_value();
                    });
                    if (closing_ && finals_.empty() &&
                        !pending_partial_.has_value()) return;
                    if (!finals_.empty()) {
                        w = std::move(finals_.front()); finals_.pop_front();
                        is_final = true;
                    } else {
                        w = std::move(*pending_partial_);
                        pending_partial_.reset();
                        is_final = false;
                    }
                }
                if (is_final) {
                    transcribe(w, fb, true);
                    std::lock_guard<std::mutex> lk(m_);
                    --pending_finals_;
                    cv_.notify_all();
                } else {
                    transcribe(w, fb, false);
                }
            }
        }

        void transcribe(const Work &w, std::vector<float> &fb, bool is_final) {
            if (w.audio.empty()) return;
            fb.resize(w.audio.size());
            for (size_t i = 0; i < w.audio.size(); ++i)
                fb[i] = static_cast<float>(w.audio[i]) / 32768.0f;

            const double rel = (double)w.start_sample / SAMPLE_RATE;
            l_->set_segment_start(rel);
            try {
                const moonshine::Transcript t =
                    t_->transcribeWithoutStreaming(fb, SAMPLE_RATE);

                std::string combined;
                double first_offset = 0.0;
                for (size_t i = 0; i < t.lines.size(); ++i) {
                    if (i == 0) first_offset = t.lines[i].startTime;
                    if (i) combined += ' ';
                    combined += t.lines[i].text;
                }
                if (combined.empty()) return;

                if (is_final) l_->emit_final(combined, rel + first_offset);
                else          l_->emit_partial(combined);
            } catch (const moonshine::MoonshineException &) {
                // swallow; onError will fire if the library reports it
            }
        }

        moonshine::Transcriber *t_;
        PrintListener          *l_;
        std::thread             th_;
        std::mutex              m_;
        std::condition_variable cv_;
        std::deque<Work>        finals_;
        int                     pending_finals_ = 0;
        std::optional<Work>     pending_partial_;
        bool                    closing_ = false;
    };

    std::unique_ptr<moonshine::Transcriber> transcriber_;
    std::unique_ptr<PrintListener>          listener_;
    std::unique_ptr<StatefulVad>            vad_;
    std::unique_ptr<InferenceThread>        inference_;
    uint64_t                                total_seen_ = 0;
    uint64_t                                last_partial_at_ = 0;
    std::vector<int16_t>                    pcm_rem_;
};

// ===========================================================================
//  Engine::Impl
// ===========================================================================

struct Engine::Impl {
    struct Slot {
        LanguageConfig                     config;
        std::unique_ptr<StreamingWorker>   stream;
        std::unique_ptr<NonStreamingWorker> offline;
        std::unique_ptr<AudioQueue>        queue;
        std::thread                        thread;

        std::shared_ptr<std::atomic<bool>>    enabled;
        std::shared_ptr<std::atomic<bool>>    need_reset;
        std::shared_ptr<std::atomic<int64_t>> disabled_at_ns;

        void feed_pcm(const std::vector<int16_t> &pcm) {
            if (!enabled || !enabled->load()) return;
            if (stream)       stream->feed(pcm);
            else if (offline) offline->feed(pcm);
        }
        void set_session_start(std::chrono::system_clock::time_point tp) {
            if (stream)       stream->set_session_start(tp);
            if (offline)      offline->set_session_start(tp);
        }
        void flush() {
            if (stream)       stream->flush();
            else if (offline) offline->flush();
        }
        void add_paused_ms(int64_t ms) {
            if (stream)       stream->add_paused_ms(ms);
            if (offline)      offline->add_paused_ms(ms);
        }
        void reset() {
            if (stream)       stream->reset();
            else if (offline) offline->reset();
        }
    };

    std::vector<LanguageConfig>           configs;
    std::vector<std::unique_ptr<Slot>>    slots;
    TranscriptSink                       *sink;
    FILE                                 *notes;
    double                                silence_rms;
    std::chrono::system_clock::time_point session_start;

    explicit Impl(const std::vector<LanguageConfig> &cfgs,
                  TranscriptSink *snk, FILE *n, double rms,
                  std::chrono::system_clock::time_point ss)
        : configs(cfgs), sink(snk), notes(n),
          silence_rms(rms), session_start(ss) {}

    void start_slot(Slot &s, int idx) {
        if (is_streaming_arch(s.config.arch)) {
            s.stream = std::make_unique<StreamingWorker>(
                s.config.model_path, s.config.arch, session_start,
                notes, sink, s.config.language, idx);
        } else {
            s.offline = std::make_unique<NonStreamingWorker>(
                s.config.model_path, s.config.arch, session_start,
                notes, sink, silence_rms, s.config.language, idx);
        }
    }

    void start_thread(Slot &s) {
        s.queue = std::make_unique<AudioQueue>(AUDIO_QUEUE_MAX_CHUNKS);
        AudioQueue *q = s.queue.get();
        s.thread = std::thread([&s, q] {
            std::vector<int16_t> chunk;
            while (q->pop(chunk)) {
                if (s.need_reset && s.need_reset->exchange(false))
                    s.reset();
                try {
                    s.feed_pcm(chunk);
                } catch (const moonshine::MoonshineException &) {
                } catch (const std::exception &) {
                }
            }
        });
    }
};

// ---------- Engine ----------

Engine::Engine(const std::vector<LanguageConfig> &configs,
               TranscriptSink *sink, FILE *notes, double silence_rms,
               std::chrono::system_clock::time_point session_start)
    : impl_(std::make_unique<Impl>(configs, sink, notes, silence_rms,
                                   session_start)) {}

Engine::~Engine() { stop(); }

void Engine::start() {
    impl_->slots.clear();
    impl_->slots.reserve(impl_->configs.size());
    for (size_t i = 0; i < impl_->configs.size(); ++i) {
        auto s = std::make_unique<Impl::Slot>();
        s->config          = impl_->configs[i];
        s->enabled         = std::make_shared<std::atomic<bool>>(true);
        s->need_reset      = std::make_shared<std::atomic<bool>>(false);
        s->disabled_at_ns  = std::make_shared<std::atomic<int64_t>>(-1);

        impl_->start_slot(*s, (int)i);
        impl_->slots.push_back(std::move(s));
    }
    for (auto &s : impl_->slots) impl_->start_thread(*s);
}

void Engine::stop() {
    if (!impl_) return;
    for (auto &s : impl_->slots) {
        if (s->queue) s->queue->close();
    }
    for (auto &s : impl_->slots) {
        if (s->thread.joinable()) s->thread.join();
    }
    // Consumer threads have exited. Now it is single-threaded, so it is safe
    // to flush each slot's in-progress VAD segment without racing a feeder.
    for (auto &s : impl_->slots) {
        try { s->flush(); } catch (...) {}
    }
    impl_->slots.clear();
}

void Engine::flush() {
    for (auto &s : impl_->slots) {
        try { s->flush(); } catch (...) {}
    }
}

void Engine::feed(const std::vector<int16_t> &pcm) {
    for (auto &s : impl_->slots) {
        if (!s->queue) continue;
        if (s->enabled && !s->enabled->load()) continue;
        s->queue->push(std::vector<int16_t>(pcm.begin(), pcm.end()));
    }
}

void Engine::toggle(int slot) {
    if (slot < 0 || slot >= (int)impl_->slots.size()) return;
    auto &s = *impl_->slots[slot];
    if (!s.enabled) return;

    const bool now = !s.enabled->load();
    s.enabled->store(now);

    const int64_t now_ns = std::chrono::duration_cast<
        std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

    if (!now) {
        if (s.need_reset)     s.need_reset->store(true);
        if (s.disabled_at_ns) s.disabled_at_ns->store(now_ns);
    } else if (s.disabled_at_ns) {
        const int64_t was = s.disabled_at_ns->exchange(-1);
        if (was >= 0) {
            const int64_t ms = (now_ns - was) / 1000000;
            if (ms > 0) s.add_paused_ms(ms);
        }
    }
}

bool Engine::is_enabled(int slot) const {
    if (slot < 0 || slot >= (int)impl_->slots.size()) return false;
    auto &s = *impl_->slots[slot];
    return s.enabled && s.enabled->load();
}

size_t Engine::size() const { return impl_->slots.size(); }

} // namespace olas
