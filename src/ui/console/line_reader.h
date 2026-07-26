#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cha {

class LineReader {
public:
    std::vector<std::string> append(std::string_view bytes);
    std::vector<std::string> flush();

private:
    void consume_line(std::string line, std::vector<std::string>& submissions);

    std::string partial_;
    std::string pending_;
    bool continuing_{};
};

} // namespace cha
