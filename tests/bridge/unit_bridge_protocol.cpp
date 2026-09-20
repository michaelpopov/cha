#include "bridge/bridge_protocol.h"

#include "web/protocol.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>

namespace cha::bridge {
namespace {

nlohmann::json load_fixture(std::string_view name) {
    const std::string path =
        std::string(CHA_WIRE_FIXTURE_DIRECTORY) + "/" + std::string(name);
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Missing wire fixture " + path);
    nlohmann::json value;
    input >> value;
    return value;
}

TEST(BridgeProtocol, ParsesSubmitEnvelopeAndRejectsMalformedRequests) {
    const auto parsed = parse_request(
        load_fixture("native-request-submit.json").dump(), 65536);
    const auto* request = std::get_if<ParsedRequest>(&parsed);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->connection_id, "view-9");
    EXPECT_EQ(request->id, 42U);
    EXPECT_EQ(request->context_epoch, 3U);
    EXPECT_EQ(request->method, Method::session_submit);
    EXPECT_EQ(request->params["input"]["text"], "Hello");

    EXPECT_EQ(method_from_name("provider.test"), Method::provider_test);
    EXPECT_EQ(method_from_name("openaiAuth.poll"), Method::openai_auth_poll);
    EXPECT_EQ(method_from_name("apiKey.replaceValue"), Method::api_key_replace_value);
    EXPECT_EQ(method_name(Method::voice_input_runtime), "voiceInput.runtime");
    EXPECT_EQ(method_from_name("speech.start"), Method::speech_start);
    EXPECT_EQ(method_from_name("audio.startBatch"), Method::audio_start_batch);
    EXPECT_EQ(method_from_name("voiceInput.connect"), Method::voice_input_connect);
    EXPECT_TRUE(is_control_method(Method::speech_cancel));
    EXPECT_TRUE(is_control_method(Method::speech_release));
    EXPECT_TRUE(is_control_method(Method::audio_release));
    EXPECT_TRUE(is_control_method(Method::voice_input_cancel));
    EXPECT_FALSE(is_control_method(Method::speech_start));

    const auto unknown_method = parse_request(
        nlohmann::json{
            {"connection_id", "view-9"},
            {"id", 1},
            {"context_epoch", 1},
            {"method", "settings.save"},
        }.dump(),
        65536);
    const auto* method_error = std::get_if<ParseFailure>(&unknown_method);
    ASSERT_TRUE(method_error);
    EXPECT_EQ(method_error->id, 1U);
    EXPECT_EQ(method_error->code, cha::web::ErrorCode::invalid_argument);
    EXPECT_EQ(method_error->message, "That method is not available.");

    const auto extra_field = parse_request(
        nlohmann::json{
            {"connection_id", "view-9"},
            {"id", 1},
            {"context_epoch", 1},
            {"method", "bridge.info"},
            {"extra", true},
        }.dump(),
        65536);
    EXPECT_TRUE(std::holds_alternative<ParseFailure>(extra_field));

    const auto unsafe_id = parse_request(
        nlohmann::json{
            {"connection_id", "view-9"},
            {"id", 9007199254740992ULL},
            {"context_epoch", 1},
            {"method", "bridge.info"},
        }.dump(),
        65536);
    EXPECT_TRUE(std::holds_alternative<ParseFailure>(unsafe_id));

    const auto too_large = parse_request("{}", 1);
    const auto* size_error = std::get_if<ParseFailure>(&too_large);
    ASSERT_TRUE(size_error);
    EXPECT_EQ(size_error->code, cha::web::ErrorCode::body_too_large);
}

TEST(BridgeProtocol, SerializesInfoRepliesEventsAndAcksAgainstFixtures) {
    EXPECT_EQ(bridge_info_result("test"), load_fixture("bridge-info.json"));
    EXPECT_EQ(
        reply_ok(
            "view-9",
            42,
            3,
            cha::web::CommandResult{
                .session = {.notice = std::string("Saved")},
                .clear_input = true}),
        load_fixture("native-reply-command.json"));
    EXPECT_EQ(
        reply_error(
            "view-9",
            42,
            3,
            cha::web::ErrorCode::session_not_live,
            public_error_message(cha::web::ErrorCode::session_not_live)),
        load_fixture("native-reply-error.json"));
    EXPECT_EQ(
        session_event(
            "view-9",
            3,
            "sub-5",
            "session.append",
            "history",
            "session-1",
            1,
            nlohmann::json{
                {"target", {{"kind", "entry"}, {"entry_id", 123}}},
                {"text", "Hello"}}),
        load_fixture("native-event-append.json"));

    const auto ack = parse_ack(
        load_fixture("native-delivery-ack.json").dump(), 65536);
    const auto* parsed = std::get_if<DeliveryAck>(&ack);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->connection_id, "view-9");
    EXPECT_EQ(parsed->delivery_id, 1U);

    EXPECT_EQ(
        connection_invalidated_event("view-9"),
        nlohmann::json({
            {"connection_id", "view-9"},
            {"event", "app.connectionInvalidated"},
            {"reason", "request_limit_exceeded"},
        }));
}

TEST(BridgeProtocol, RejectsUnknownMethodsAndKeepsVersionFixed) {
    EXPECT_EQ(kProtocolVersion, 1);
    EXPECT_EQ(method_from_name("bridge.info"), Method::bridge_info);
    EXPECT_FALSE(method_from_name("executeShell"));
    EXPECT_FALSE(requires_context_epoch(Method::bridge_info));
    EXPECT_FALSE(requires_context_epoch(Method::app_bootstrap));
    EXPECT_TRUE(requires_context_epoch(Method::session_submit));
    EXPECT_TRUE(is_control_method(Method::session_stop));
    EXPECT_TRUE(is_control_method(Method::session_unsubscribe));
    EXPECT_TRUE(is_control_method(Method::session_close));
    EXPECT_FALSE(is_control_method(Method::session_submit));
}

} // namespace
} // namespace cha::bridge
