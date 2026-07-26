#pragma once

#include "ui/render/transcript_writer.h"
#include "util/wake_notifier.h"

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace cha {

// The semantic events returned by one frontend wait. Platform-specific handle,
// signal, and stream details stay inside the ConsolePort implementation.
class InputEvents {
public:
    static InputEvents ready(
        bool input = false,
        bool input_closed = false,
        bool notification = false,
        bool signal = false) {
        return InputEvents(
            false,
            input,
            input_closed,
            notification,
            signal);
    }

    static InputEvents failure() {
        return InputEvents(true, false, false, false, false);
    }

    bool failed() const { return failed_; }
    bool input_ready() const { return input_; }
    bool input_closed() const { return input_closed_; }
    bool notification_ready() const { return notification_; }
    bool signal_ready() const { return signal_; }

private:
    InputEvents(
        bool failed,
        bool input,
        bool input_closed,
        bool notification,
        bool signal)
        : failed_(failed),
          input_(input),
          input_closed_(input_closed),
          notification_(notification),
          signal_(signal) {
    }

    bool failed_{};
    bool input_{};
    bool input_closed_{};
    bool notification_{};
    bool signal_{};
};

// Everything the console loop needs from the outside world. wait() reports
// non-sticky semantic events; input_closed() is sticky after EOF. The port is
// also the agent thread's wake target, so its concrete event loop never leaks
// into ConsoleSession. When include_input is false, wait() must suppress both
// input-ready and input-closed delivery, and input_closed() must not become
// sticky for an EOF whose buffered input is still deferred. This lets a
// non-interactive producer receive backpressure while agent and signal events
// remain enabled.
class ConsolePort : public WakeNotifier {
public:
    virtual ~ConsolePort() = default;

    virtual InputEvents wait(bool include_input = true) = 0;
    virtual std::vector<std::string> take_lines() = 0;
    virtual bool input_closed() const = 0;
    virtual bool take_interrupt() = 0;

    virtual TranscriptSurface& transcript() = 0;
    // A separate attributed surface for the interactive prompt on stderr.
    virtual TranscriptSurface& prompt() = 0;
    virtual std::ostream& notices() = 0;
    [[nodiscard]] virtual bool flush() = 0;
    // Ends the transcript byte stream and checks delivery of any sanitizer
    // state that could not be emitted by an ordinary flush.
    [[nodiscard]] virtual bool finish_transcript() = 0;
};

} // namespace cha
