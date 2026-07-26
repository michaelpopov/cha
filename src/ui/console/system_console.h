#pragma once

#include "ui/console/console_port.h"
#include "ui/console/console_writer.h"
#include "ui/console/line_reader.h"

namespace cha {

class SystemConsole final : public ConsolePort {
public:
    SystemConsole(int signal_fd, bool color);
    ~SystemConsole() override;

    SystemConsole(const SystemConsole&) = delete;
    SystemConsole& operator=(const SystemConsole&) = delete;

    InputEvents wait(
        int notification_fd,
        bool include_input = true) override;
    std::vector<std::string> take_lines() override;
    bool input_closed() const override;
    bool take_interrupt() override;
    TranscriptSurface& transcript() override;
    std::ostream& notices() override;
    bool flush() override;

private:
    int signal_fd_{-1};
    int original_input_flags_{-1};
    bool input_closed_{};
    bool may_read_{};
    LineReader reader_;
    ConsoleSurface surface_;
};

} // namespace cha
