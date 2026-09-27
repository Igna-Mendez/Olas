#pragma once
#include <string>

namespace olas {

// One update from the engine to the UI. Emitted on a worker thread;
// the sink is responsible for marshalling to its own thread.
struct TranscriptEvent {
    int         slot     = 0;
    std::string prefix;              // "[HH:MM:SS] [lang] " (stamp tag)
    std::string body;                // text (body tag)
    bool        is_final = false;    // commit vs. replace-current-partial
    bool        is_error = false;    // error line
};

class TranscriptSink {
public:
    virtual ~TranscriptSink() = default;
    virtual void post(TranscriptEvent ev) = 0;
};

} // namespace olas