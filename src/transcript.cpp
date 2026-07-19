#include "transcript.h"

#include <utility>

namespace cha {

void Transcript::add_user(std::string text) {
    finish_assistant();
    entries_.push_back(TranscriptEntry{Speaker::user, std::move(text)});
    ++revision_;
}

void Transcript::begin_assistant() {
    finish_assistant();
    entries_.push_back(TranscriptEntry{Speaker::assistant, {}});
    assistant_open_ = true;
    ++revision_;
}

void Transcript::append_assistant(std::string_view text) {
    if (!assistant_open_) {
        begin_assistant();
    }
    entries_.back().text.append(text);
    ++revision_;
}

void Transcript::finish_assistant() {
    assistant_open_ = false;
}

void Transcript::add_system(std::string text) {
    finish_assistant();
    entries_.push_back(TranscriptEntry{Speaker::system, std::move(text)});
    ++revision_;
}

const std::vector<TranscriptEntry>& Transcript::entries() const {
    return entries_;
}

std::size_t Transcript::revision() const {
    return revision_;
}

} // namespace cha
