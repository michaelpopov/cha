#pragma once

#include "ui/render/transcript_writer.h"

#include <iosfwd>
#include <string>
#include <string_view>

namespace cha {

class ConsoleSurface final : public TranscriptSurface {
public:
    ConsoleSurface(std::ostream& output, bool color);

    void attributes(TranscriptAttributes value) override;
    void write(std::string_view text) override;

private:
    std::ostream& output_;
    bool color_{};
};

std::string sanitize_console_text(std::string_view text);

} // namespace cha
