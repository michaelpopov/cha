#include "agents/sse_framer.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace cha {
namespace {

TEST(SseFramer, DecodesAStreamOneByteAtATimeAndNormalizesSplitCarriageReturns) {
    SseFramer framer;
    std::vector<std::string> data;
    const std::string stream = "data: first\r\n\r\ndata:   second\r\n\r\n";

    for (const char character : stream) {
        framer.consume(std::string_view(&character, 1), [&data](std::string_view value) {
            data.emplace_back(value);
            return true;
        });
    }

    EXPECT_EQ(data, (std::vector<std::string>{"first", "second"}));
}

TEST(SseFramer, DecodesTheTrailingUnterminatedEventWhenFinished) {
    SseFramer framer;
    std::vector<std::string> data;

    framer.consume("event: ignored\ndata: tail", [&data](std::string_view value) {
        data.emplace_back(value);
        return true;
    });
    framer.finish([&data](std::string_view value) {
        data.emplace_back(value);
        return true;
    });

    EXPECT_EQ(data, (std::vector<std::string>{"tail"}));
}

TEST(SseFramer, StopsAtTheHandlerAndDiscardsTheRestOfTheChunk) {
    SseFramer framer;
    std::vector<std::string> data;

    framer.consume("data: first\n\ndata: second\n\n", [&data](std::string_view value) {
        data.emplace_back(value);
        return false;
    });

    EXPECT_EQ(data, (std::vector<std::string>{"first"}));
}

} // namespace
} // namespace cha
