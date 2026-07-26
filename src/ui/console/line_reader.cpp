#include "ui/console/line_reader.h"

#include <utility>

namespace cha {

std::vector<std::string> LineReader::append(std::string_view bytes) {
    partial_.append(bytes);
    std::vector<std::string> submissions;
    std::size_t newline = partial_.find('\n');
    while (newline != std::string::npos) {
        std::string line = partial_.substr(0, newline);
        partial_.erase(0, newline + 1);
        consume_line(std::move(line), submissions);
        newline = partial_.find('\n');
    }
    return submissions;
}

std::vector<std::string> LineReader::flush() {
    std::vector<std::string> submissions;
    if (partial_.empty() && !continuing_) {
        return submissions;
    }
    consume_line(std::move(partial_), submissions);
    partial_.clear();
    if (continuing_) {
        submissions.push_back(std::move(pending_));
        pending_.clear();
        continuing_ = false;
    }
    return submissions;
}

void LineReader::consume_line(
    std::string line,
    std::vector<std::string>& submissions) {
    const bool continues = !line.empty() && line.back() == '\\';
    if (continues) {
        line.pop_back();
    }

    if (continuing_) {
        pending_ += line;
    } else if (continues) {
        pending_ = std::move(line);
    }

    if (continues) {
        continuing_ = true;
        return;
    }
    if (continuing_) {
        submissions.push_back(std::move(pending_));
        pending_.clear();
        continuing_ = false;
    } else {
        submissions.push_back(std::move(line));
    }
}

} // namespace cha
