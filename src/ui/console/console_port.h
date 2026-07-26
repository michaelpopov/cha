#pragma once

#include "ui/render/transcript_writer.h"
#include "util/input_wait.h"

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace cha {

// Everything the console loop needs from the outside world. wait() reports
// non-sticky poll bits; input_closed() is sticky after take_lines() observes
// EOF. The include_input flag lets a non-interactive producer receive
// backpressure while agent and signal descriptors remain pollable.
class ConsolePort {
public:
    virtual ~ConsolePort() = default;

    virtual InputEvents wait(
        int notification_fd,
        bool include_input = true) = 0;
    virtual std::vector<std::string> take_lines() = 0;
    virtual bool input_closed() const = 0;
    virtual bool take_interrupt() = 0;

    virtual TranscriptSurface& transcript() = 0;
    virtual std::ostream& notices() = 0;
    [[nodiscard]] virtual bool flush() = 0;
};

} // namespace cha
