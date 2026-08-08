#include "web/sse_stream.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace cha::web {
namespace {

using namespace std::chrono_literals;

SessionSnapshot snapshot_with_text(std::string text) {
    return {
        .transcript = {{
            .id = 42,
            .kind = EntryKind::error,
            .text = std::move(text),
            .status = EntryStatus::complete,
        }},
    };
}

struct CapturingDataSink {
    CapturingDataSink() {
        sink.write = [this](const char* data, std::size_t size) {
            content.append(data, size);
            return write_succeeds;
        };
        sink.is_writable = [] { return true; };
        sink.done = [this] { done = true; };
        sink.done_with_trailer = [this](const httplib::Headers&) { done = true; };
    }

    httplib::DataSink sink;
    std::string content;
    bool write_succeeds{true};
    bool done{};
};

TEST(SseStream, FramesOnlyContractRecords) {
    const std::string snapshot = sse_frame(
        SsePayload{SnapshotEvent{snapshot_with_text("line\ntext")}});
    EXPECT_TRUE(snapshot.starts_with("event: snapshot\n"));
    EXPECT_TRUE(snapshot.ends_with("\n\n"));
    EXPECT_EQ(snapshot.find("id:"), std::string::npos);
    EXPECT_EQ(snapshot.find("retry:"), std::string::npos);
    const std::size_t data = snapshot.find("data: ");
    ASSERT_NE(data, std::string::npos);
    EXPECT_EQ(snapshot.find('\n', data), snapshot.size() - 2);
    EXPECT_EQ(
        nlohmann::json::parse(snapshot.substr(data + 6))
            .at("transcript")[0]
            .at("text"),
        "line\ntext");
    EXPECT_EQ(sse_heartbeat(), ": heartbeat\n\n");
}

TEST(SseStreamWriter, SerializationFailureClosesOnlyTheStream) {
    auto mailbox = std::make_shared<SseMailbox>();
    std::string invalid_utf8(1, static_cast<char>(0xff));
    const SseMailbox::Stream stream =
        mailbox->begin_stream({snapshot_with_text(std::move(invalid_utf8))});
    SseStreamWriter writer(mailbox, stream, 1ms);
    CapturingDataSink output;

    bool result = true;
    EXPECT_NO_THROW(result = writer.write(output.sink));
    EXPECT_FALSE(result);
    EXPECT_TRUE(output.content.empty());
    EXPECT_FALSE(output.done);
}

TEST(SseStreamWriter, ClosedMailboxEndsChunkedResponseCleanly) {
    auto mailbox = std::make_shared<SseMailbox>();
    const SseMailbox::Stream stream =
        mailbox->begin_stream({snapshot_with_text("final")});
    mailbox->close();
    SseStreamWriter writer(mailbox, stream, 1ms);
    CapturingDataSink output;

    EXPECT_TRUE(writer.write(output.sink));
    EXPECT_TRUE(output.done);
    EXPECT_TRUE(output.content.empty());
}

TEST(SseStreamWriter, IdleOpenStreamEmitsHeartbeat) {
    auto mailbox = std::make_shared<SseMailbox>();
    const SseMailbox::Stream stream =
        mailbox->begin_stream({snapshot_with_text("initial")});
    SseStreamWriter writer(mailbox, stream, 1ms);
    CapturingDataSink output;
    ASSERT_TRUE(writer.write(output.sink));
    output.content.clear();

    EXPECT_TRUE(writer.write(output.sink));
    EXPECT_EQ(output.content, sse_heartbeat());
    EXPECT_FALSE(output.done);
}

TEST(SseStreamWriter, FailedWriteCancelsWithoutAcknowledgingPayload) {
    auto mailbox = std::make_shared<SseMailbox>();
    const SseMailbox::Stream stream =
        mailbox->begin_stream({snapshot_with_text("initial")});
    SseStreamWriter writer(mailbox, stream, 1ms);
    CapturingDataSink output;
    output.write_succeeds = false;

    EXPECT_FALSE(writer.write(output.sink));
    EXPECT_FALSE(mailbox->wait_for_written(1ms));
    EXPECT_FALSE(output.done);
}

} // namespace
} // namespace cha::web
