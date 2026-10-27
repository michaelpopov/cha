#include "providers/anthropic_api.h"
#include "providers/tool_calls.h"
#include "chat/transcript.h"
#include "support/test_transcript.h"

#include <gtest/gtest.h>
#include <limits>

namespace cha {
namespace {
using Json = nlohmann::json;

Json response(Json content, std::string reason = "end_turn") {
    return {{"type", "message"}, {"role", "assistant"}, {"content", std::move(content)}, {"stop_reason", reason}};
}
Json call(std::string id = "toolu_1", Json input = Json::object()) {
    return {{"type", "tool_use"}, {"id", id}, {"name", "assistant_events"}, {"input", std::move(input)}};
}
std::string event(const Json& value) { return "data: " + value.dump() + "\n\n"; }
std::string start(Json usage = Json::object()) {
    auto message = response(Json::array());
    message["stop_reason"] = nullptr;
    message["usage"] = usage;
    return event({{"type", "message_start"}, {"message", message}});
}
std::string terminal(std::string reason, Json usage = Json::object()) {
    return event({{"type", "message_delta"}, {"delta", {{"stop_reason", reason}}}, {"usage", usage}})
        + event({{"type", "message_stop"}});
}
std::string block_start(int index, Json block) {
    return event({{"type", "content_block_start"}, {"index", index}, {"content_block", std::move(block)}});
}
std::string block_stop(int index) { return event({{"type", "content_block_stop"}, {"index", index}}); }
std::string input_delta(std::string fragment) {
    return event({{"type", "content_block_delta"}, {"index", 0},
        {"delta", {{"type", "input_json_delta"}, {"partial_json", fragment}}}});
}
std::string stream_response(const Json& message) {
    std::string stream = start(message.value("usage", Json::object()));
    int index = 0;
    for (const auto& block : message.at("content")) {
        stream += block_start(index, block) + block_stop(index);
        ++index;
    }
    return stream + terminal(message.at("stop_reason"));
}
struct Output {
    std::string answer, reasoning;
    GenerationDeltaSink sink = [this](GenerationDelta delta) {
        (delta.kind == GenerationDeltaKind::answer ? answer : reasoning) += delta.text;
    };
};

TEST(AnthropicApi, ConfigurationAndCustomEndpointsIgnoreUnusedValues) {
    EXPECT_EQ(parse_provider_api("anthropic"), ProviderApi::anthropic);
    EXPECT_EQ(to_string(ProviderApi::anthropic), "anthropic");
    ModelBackendConfig config{.host = "api.anthropic.com", .port = 443,
        .mode = Mode::net, .model = "arbitrary-model-id", .https = true, .api = ProviderApi::anthropic};
    EXPECT_FALSE(provider_config_error(config));
    EXPECT_EQ(provider_endpoint(config), "https://api.anthropic.com:443/v1/messages");
    config.host = "::1"; config.port = 8080; config.https = false; config.base_path = "/proxy";
    EXPECT_EQ(provider_endpoint(config), "http://[::1]:8080/proxy/v1/messages");
    config.temperature = std::numeric_limits<double>::quiet_NaN();
    config.reasoning_effort = "obsolete";
    config.openrouter_targets = {"bad target", "bad target"};
    config.web_search = WebSearchMode::required;
    EXPECT_FALSE(provider_config_error(config));
    EXPECT_TRUE(provider_supports_reasoning_effort(config, config.api));
    config.host = "openrouter.ai";
    EXPECT_FALSE(provider_supports_web_search(config));
    config.max_tokens = 0;
    EXPECT_TRUE(provider_config_error(config));
    config.max_tokens = 100;
    config.auth = ProviderAuth::openai_subscription;
    EXPECT_TRUE(provider_config_error(config));
}

TEST(AnthropicApi, ProjectsHistoryAndTranslatesTheCompleteSharedToolCatalog) {
    Transcript transcript;
    transcript.add_entry(make_character_entry(1, "assistant", "Assistant", "Retained reply", EntryStatus::complete, 1));
    transcript.add_entry(make_character_entry(2, "assistant", "Assistant", "Another reply", EntryStatus::complete, 2));
    GenerationRequest request{
        .history = std::make_shared<const ModelHistory>(transcript.model_history()),
        .run = {.request_id = 3, .target = {"assistant", "Assistant"},
            .author = {"human", "You"}, .prompt_text = "Current question"},
        .maintenance_tool = [](auto, auto, const auto&) { return "{}"; },
    };
    ModelBackendConfig config{.model = "claude-test", .temperature = 0.5,
        .reasoning_effort = "high", .api = ProviderApi::anthropic,
        .web_search = WebSearchMode::required, .openrouter_targets = {"ignored"}};
    RequestTextSizes sizes;
    auto body = Json::parse(build_anthropic_request_body(request, config, "System prompt", &sizes));
    const auto projected = project_model_context(request, "System prompt");
    EXPECT_EQ(body["model"], "claude-test");
    EXPECT_EQ(body["stream"], true);
    EXPECT_EQ(body["max_tokens"], 64000);
    EXPECT_EQ(body["messages"].front()["role"], "assistant");
    ASSERT_EQ(body["messages"].front()["content"].size(), 2u);
    EXPECT_EQ(body["messages"][0]["content"][0]["text"], projected[1].content);
    EXPECT_EQ(body["messages"].back()["role"], "user");
    EXPECT_EQ(body["system"].get<std::string>().size(), sizes.system_prompt_bytes);
    EXPECT_GT(sizes.conversation_bytes, 0u);
    for (const auto* field : {"temperature", "top_p", "top_k", "reasoning", "reasoning_effort",
             "reasoning_format", "cache_retention", "provider", "thinking", "tool_choice", "instructions"}) {
        EXPECT_FALSE(body.contains(field)) << field;
    }
    Json shared;
    add_maintenance_tools(shared, ProviderApi::responses);
    ASSERT_EQ(body["tools"].size(), shared["tools"].size());
    for (std::size_t i = 0; i < body["tools"].size(); ++i) {
        EXPECT_EQ(body["tools"][i]["name"], shared["tools"][i]["name"]);
        EXPECT_EQ(body["tools"][i]["input_schema"], shared["tools"][i]["parameters"]);
        EXPECT_EQ(body["tools"][i].size(), 3u);
    }
    config.max_tokens = 123; config.stream = false;
    request.include_tool_instructions = false; request.maintenance_tool = {};
    body = Json::parse(build_anthropic_request_body(request, config, "Auxiliary"));
    EXPECT_EQ(body["system"], "Auxiliary");
    EXPECT_EQ(body["max_tokens"], 123);
    EXPECT_EQ(body["stream"], false);
    EXPECT_FALSE(body.contains("tools"));
    request.include_tool_instructions = true;
    request.web_search_tool = [](auto, const auto&) { return ""; };
    request.web_read_tool = [](auto, const auto&, const auto&) { return ""; };
    body = Json::parse(build_anthropic_request_body(request, config, ""));
    EXPECT_NE(body["system"].get<std::string>().find("Web search is available"), std::string::npos);
    EXPECT_NE(body["system"].get<std::string>().find("Web page reading is available"), std::string::npos);
}

TEST(AnthropicApi, JsonAndEverySseSplitPreserveThinkingTextAndCumulativeUsage) {
    const auto content = Json::array({
        {{"type", "thinking"}, {"thinking", "Visible summary"}, {"signature", "signed"}},
        {{"type", "redacted_thinking"}, {"data", "opaque"}},
        {{"type", "text"}, {"text", "Checking."}}, call(), call("toolu_2", {{"x", 1}})});
    auto message = response(content, "tool_use");
    message["usage"] = {{"input_tokens", 10}, {"output_tokens", 7},
        {"cache_read_input_tokens", 4}, {"cache_creation_input_tokens", 2}};
    Output full;
    const auto expected = decode_anthropic_response(message.dump(), full.sink, true);
    ASSERT_EQ(expected.outcome, GenerationOutcome::completed) << expected.message;
    EXPECT_EQ(Json::parse(expected.continuation), (Json{{"role", "assistant"}, {"content", content}}));
    EXPECT_EQ(expected.tool_calls.size(), 2u);
    EXPECT_EQ(expected.usage.input_tokens, 16u);
    auto stream = start({{"input_tokens", 9}, {"output_tokens", 1}, {"cache_read_input_tokens", 4}});
    stream += block_start(0, {{"type", "thinking"}, {"thinking", ""}, {"signature", ""}});
    for (const auto& [type, field, text] : {std::tuple{"thinking_delta", "thinking", "Visible summary"},
             {"signature_delta", "signature", "signed"}}) {
        stream += event({{"type", "content_block_delta"}, {"index", 0}, {"delta", {{"type", type}, {field, text}}}});
    }
    stream += block_stop(0);
    for (int i = 1; i < 5; ++i) {
        if (i == 2) {
            stream += block_start(i, {{"type", "text"}, {"text", ""}});
            for (const auto* fragment : {"Check", "ing."}) {
                stream += event({{"type", "content_block_delta"}, {"index", i},
                    {"delta", {{"type", "text_delta"}, {"text", fragment}}}});
            }
            stream += block_stop(i);
        } else stream += block_start(i, content[i]) + block_stop(i);
    }
    stream += event({{"type", "ping"}}) + event({{"type", "future_event"}});
    stream += terminal("tool_use", {{"input_tokens", 10}, {"output_tokens", 7}, {"cache_creation_input_tokens", 2}});
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        Output output;
        AnthropicStreamDecoder decoder(output.sink, true);
        decoder.consume(std::string_view(stream).substr(0, split));
        decoder.consume(std::string_view(stream).substr(split));
        const auto result = decoder.finish().result;
        ASSERT_EQ(result.outcome, expected.outcome) << result.message << " split=" << split;
        EXPECT_EQ(result.continuation, expected.continuation);
        EXPECT_EQ(result.usage.input_tokens, 16u);
        EXPECT_EQ(result.usage.output_tokens, 7u);
        EXPECT_EQ(result.usage.cache_read_tokens, 4u);
        EXPECT_EQ(result.usage.cache_write_tokens, 2u);
        EXPECT_EQ(output.answer, "Checking.");
        EXPECT_EQ(output.reasoning, "Visible summary");
    }
}

TEST(AnthropicApi, FragmentedArgumentsAndNoArgumentCalls) {
    Output output;
    AnthropicStreamDecoder decoder(output.sink, true);
    const auto stream = start() + block_start(0, call())
        + input_delta("") + input_delta("{\"q") + input_delta("uery\":\"hello\"}") + block_stop(0) + terminal("tool_use");
    for (const char byte : stream) decoder.consume(std::string_view(&byte, 1));
    const auto result = decoder.finish().result;
    ASSERT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
    ASSERT_EQ(result.tool_calls.size(), 1u);
    EXPECT_EQ(result.tool_calls[0].arguments, R"({"query":"hello"})");
    for (const auto& deltas : {std::string{}, input_delta(""), input_delta("") + input_delta("")}) {
        AnthropicStreamDecoder empty(output.sink, true);
        empty.consume(start() + block_start(0, call()) + deltas + block_stop(0) + terminal("tool_use"));
        const auto no_arguments = empty.finish().result;
        ASSERT_EQ(no_arguments.outcome, GenerationOutcome::completed) << no_arguments.message;
        ASSERT_EQ(no_arguments.tool_calls.size(), 1u);
        EXPECT_EQ(no_arguments.tool_calls[0].arguments, "{}");
        EXPECT_EQ(Json::parse(no_arguments.continuation)["content"][0]["input"], Json::object());
    }
}

TEST(AnthropicApi, TerminalReasonsRetainTextAndNeverExecuteUnsafeCalls) {
    for (const auto* reason : {"end_turn", "stop_sequence", "tool_use", "max_tokens",
             "model_context_window_exceeded", "refusal", "pause_turn", "future_reason", ""}) {
        for (const bool calls : {false, true}) {
            const bool length = std::string_view(reason) == "max_tokens" || std::string_view(reason) == "model_context_window_exceeded";
            const bool refusal = std::string_view(reason) == "refusal";
            const bool success = calls ? std::string_view(reason) == "tool_use"
                : std::string_view(reason) == "end_turn" || std::string_view(reason) == "stop_sequence" || length || refusal;
            auto content = Json::array({{{"type", "text"}, {"text", "Partial"}}});
            if (calls) content.push_back(call());
            for (const bool stream : {false, true}) {
                Output output;
                const auto message = response(content, reason);
                GenerationResult result;
                if (stream) {
                    AnthropicStreamDecoder decoder(output.sink, true, maintenance_argument_limit, true);
                    decoder.consume(stream_response(message));
                    result = decoder.finish().result;
                } else result = decode_anthropic_response(message.dump(), output.sink, true, maintenance_argument_limit, true);
                EXPECT_EQ(output.answer, "Partial");
                EXPECT_EQ(result.outcome == GenerationOutcome::completed, success) << reason;
                EXPECT_EQ(result.refused, refusal);
                if (!success) EXPECT_TRUE(result.tool_calls.empty());
                if (length && calls) EXPECT_EQ(result.message, maintenance_output_limit_message);
                if (length) EXPECT_EQ(result.finish_reason, "length");
                if (refusal) EXPECT_EQ(result.finish_reason, "content_filter");
            }
        }
    }
}

TEST(AnthropicApi, TruncationPrecedesInvalidOrOversizedToolInput) {
    for (const bool maintenance : {false, true}) {
        for (const auto* reason : {"max_tokens", "model_context_window_exceeded"}) {
            for (const auto& fragment : {std::string("{bad"), std::string(101, 'x')}) {
                for (const bool closed : {false, true}) {
                    Output output;
                    AnthropicStreamDecoder decoder(output.sink, true, 100, maintenance);
                    decoder.consume(start() + block_start(0, call()) + input_delta(fragment)
                        + (closed ? block_stop(0) : "") + terminal(reason));
                    const auto result = decoder.finish().result;
                    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
                    EXPECT_EQ(result.message, maintenance ? maintenance_output_limit_message : "Tool response ended incomplete");
                    EXPECT_TRUE(result.tool_calls.empty());
                }
                Output output;
                auto value = call(); value["input"] = fragment;
                const auto result = decode_anthropic_response(response(Json::array({value}), reason).dump(), output.sink, true, 100, maintenance);
                EXPECT_EQ(result.message, maintenance ? maintenance_output_limit_message : "Tool response ended incomplete");
            }
        }
    }
    Output output;
    AnthropicStreamDecoder decoder(output.sink, true, 100);
    decoder.consume(start() + block_start(0, call()) + input_delta(std::string(60, 'x'))
        + input_delta(std::string(60, 'x')) + input_delta("still ignored") + block_stop(0) + terminal("tool_use"));
    EXPECT_EQ(decoder.finish().result.message, "Tool arguments too large");
    auto large = response(Json::array({call("id", {{"content", std::string(65536, 'a')}})}), "tool_use");
    EXPECT_EQ(decode_anthropic_response(large.dump(), output.sink, true).message, "Tool arguments too large");
    EXPECT_EQ(decode_anthropic_response(large.dump(), output.sink, true, maintenance_argument_limit, true).outcome, GenerationOutcome::completed);
}

TEST(AnthropicApi, MalformedEventsMissingTerminationAndInvalidToolIdsFail) {
    for (const auto& stream : {
             start() + block_start(0, call()) + input_delta("[]") + block_stop(0) + terminal("tool_use"),
             start() + block_start(1, call()) + terminal("tool_use"),
             start() + block_stop(0) + terminal("end_turn"),
             start() + block_start(0, call()) + terminal("tool_use"),
             start() + event({{"type", "content_block_delta"}, {"index", -1}}),
             start() + event({{"type", "message_delta"}, {"delta", "bad"}}),
             start() + block_start(0, {{"type", "text"}, {"text", "Delivered"}}) + block_stop(0),
             start() + event({{"type", "message_delta"}, {"delta", {{"stop_reason", "end_turn"}}}}),
             start() + event({{"type", "message_stop"}}),
             std::string("data: {bad}\n\n")}) {
        Output output;
        AnthropicStreamDecoder decoder(output.sink, true);
        decoder.consume(stream);
        EXPECT_EQ(decoder.finish().result.outcome, GenerationOutcome::protocol_error) << stream;
    }
    for (const auto& message : {Json::object(), Json::array(), response(Json::array()),
             response(Json::array({call("same"), call("same")}), "tool_use"),
             response(Json::array({call("")}), "tool_use"),
             response(Json::array({42})), response(Json::array({{{"type", "future_block"}}}))}) {
        Output output;
        EXPECT_EQ(decode_anthropic_response(message.dump(), output.sink, true).outcome, GenerationOutcome::protocol_error);
    }
    Output output;
    AnthropicStreamDecoder decoder(output.sink);
    decoder.consume(start() + block_start(0, {{"type", "text"}, {"text", "Delivered"}}) + block_stop(0)
        + event({{"type", "error"}, {"error", {{"message", "Provider diagnostic"}}}}));
    EXPECT_EQ(decoder.finish().result.message, "Provider diagnostic");
    EXPECT_EQ(output.answer, "Delivered");
}

TEST(AnthropicApi, MissingAndNullUsageCountsRemainUnknown) {
    Output output;
    auto message = response(Json::array({{{"type", "text"}, {"text", "OK"}}}));
    for (const auto& usage : {Json::object(), Json{{"input_tokens", nullptr}, {"output_tokens", nullptr},
             {"cache_read_input_tokens", nullptr}, {"cache_creation_input_tokens", nullptr}}}) {
        message["usage"] = usage;
        const auto result = decode_anthropic_response(message.dump(), output.sink);
        EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
        EXPECT_FALSE(result.usage.input_tokens);
        EXPECT_FALSE(result.usage.output_tokens);
        EXPECT_FALSE(result.usage.cache_read_tokens);
        EXPECT_FALSE(result.usage.cache_write_tokens);
    }
    message["usage"] = {{"input_tokens", 3}, {"cache_read_input_tokens", nullptr},
        {"cache_creation_input_tokens", nullptr}};
    const auto result = decode_anthropic_response(message.dump(), output.sink);
    EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
    EXPECT_EQ(result.usage.input_tokens, 3u);
    EXPECT_FALSE(result.usage.cache_read_tokens);
    EXPECT_FALSE(result.usage.cache_write_tokens);
}

TEST(AnthropicApi, NullStreamUsagePreservesLatestReportedCounts) {
    Output output;
    AnthropicStreamDecoder decoder(output.sink);
    decoder.consume(start({{"input_tokens", 10}, {"output_tokens", 1},
            {"cache_read_input_tokens", nullptr}, {"cache_creation_input_tokens", nullptr}})
        + block_start(0, {{"type", "text"}, {"text", "OK"}}) + block_stop(0)
        + event({{"type", "message_delta"}, {"delta", Json::object()},
            {"usage", {{"input_tokens", nullptr}, {"output_tokens", 2},
                {"cache_read_input_tokens", 4}, {"cache_creation_input_tokens", 2}}}})
        + terminal("end_turn", {{"input_tokens", nullptr}, {"output_tokens", 42},
            {"cache_read_input_tokens", nullptr}, {"cache_creation_input_tokens", nullptr}}));
    const auto result = decoder.finish().result;
    EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
    EXPECT_EQ(output.answer, "OK");
    EXPECT_EQ(result.usage.input_tokens, 16u);
    EXPECT_EQ(result.usage.output_tokens, 42u);
    EXPECT_EQ(result.usage.cache_read_tokens, 4u);
    EXPECT_EQ(result.usage.cache_write_tokens, 2u);
}

} // namespace
} // namespace cha
