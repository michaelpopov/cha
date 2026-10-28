#include "providers/google_api.h"
#include "providers/tool_calls.h"
#include "chat/transcript.h"
#include "support/test_transcript.h"

#include <gtest/gtest.h>

namespace cha {
namespace {
using Json = nlohmann::json;

Json response(Json parts, std::string reason = "STOP") {
    return {{"candidates", Json::array({{
        {"index", 0}, {"content", {{"role", "model"}, {"parts", std::move(parts)}}},
        {"finishReason", std::move(reason)}}})}};
}

class Output {
public:
    std::string answer;
    std::string reasoning;
    GenerationDeltaSink sink = [this](GenerationDelta delta) {
        (delta.kind == GenerationDeltaKind::answer ? answer : reasoning) += delta.text;
    };
};

TEST(GoogleApi, NativeConfigurationAndEndpoints) {
    EXPECT_EQ(parse_provider_api("google"), ProviderApi::google);
    EXPECT_EQ(to_string(ProviderApi::google), "google");
    ModelBackendConfig config{.host = "generativelanguage.googleapis.com", .port = 443,
        .base_path = "/v1beta", .mode = Mode::net, .model = "gemini-test",
        .https = true, .api = ProviderApi::google};
    EXPECT_FALSE(provider_config_error(config));
    for (const auto& model : {"gemini-test", "models/gemini-test"}) {
        config.model = model;
        EXPECT_EQ(provider_endpoint(config),
            "https://generativelanguage.googleapis.com:443/v1beta/models/gemini-test:streamGenerateContent?alt=sse");
    }
    config.stream = false;
    config.host = "::1";
    config.port = 8080;
    config.https = false;
    EXPECT_EQ(provider_endpoint(config), "http://[::1]:8080/v1beta/models/gemini-test:generateContent");
    config.reasoning_effort = "obsolete";
    config.openrouter_targets = {"bad target", "bad target"};
    config.web_search = WebSearchMode::required;
    EXPECT_FALSE(provider_config_error(config));
    EXPECT_TRUE(provider_supports_reasoning_effort(config, config.api));
    EXPECT_FALSE(provider_supports_web_search(config));
    for (const auto& model : {"models/", "models/models/test", "a/b", "../x", "x?key=y", "x#y", "x%2fy", "x\\y", "x:y", "x y"}) {
        config.model = model;
        EXPECT_TRUE(provider_config_error(config)) << model;
        EXPECT_THROW(provider_endpoint(config), std::invalid_argument);
    }
}

TEST(GoogleApi, EncodesProjectedHistoryOptionsAndSharedTools) {
    Transcript transcript;
    transcript.add_entry(make_character_entry(1, "assistant", "Assistant", "Retained reply",
        EntryStatus::complete, 1));
    transcript.add_entry(make_character_entry(2, "assistant", "Assistant", "Another reply",
        EntryStatus::complete, 2));
    GenerationRequest request{
        .history = std::make_shared<const ModelHistory>(transcript.model_history()),
        .run = {.request_id = 3, .target = {"assistant", "Assistant"},
            .author = {"human", "You"}, .prompt_text = "Current question"},
        .maintenance_tool = [](auto, auto, const auto&) { return "{}"; },
    };
    ModelBackendConfig config{.model = "gemini", .temperature = 0.5, .max_tokens = 123,
        .reasoning_effort = "high", .api = ProviderApi::google,
        .web_search = WebSearchMode::required, .openrouter_targets = {"ignored"}};
    RequestTextSizes sizes;
    const auto body = Json::parse(build_google_request_body(request, config, "System prompt", &sizes));
    const auto projected = project_model_context(request, "System prompt");
    EXPECT_EQ(body.at("contents").front().at("role"), "model");
    EXPECT_EQ(body.at("contents").front().at("parts").size(), 2u);
    EXPECT_EQ(body.at("contents").front().at("parts")[0]["text"], projected[1].content);
    EXPECT_EQ(body.at("contents").back().at("role"), "user");
    EXPECT_EQ(body.at("generationConfig"), (Json{{"candidateCount", 1}, {"temperature", 0.5}, {"maxOutputTokens", 123}}));
    EXPECT_EQ(sizes.system_prompt_bytes, body["systemInstruction"]["parts"][0]["text"].get<std::string>().size());
    EXPECT_GT(sizes.conversation_bytes, 0u);
    for (const auto* field : {"messages", "model", "stream", "tool_choice", "reasoning_effort", "reasoning", "provider", "thinkingConfig"}) {
        EXPECT_FALSE(body.contains(field)) << field;
    }
    Json shared;
    add_maintenance_tools(shared, ProviderApi::responses);
    const auto& declarations = body.at("tools")[0].at("functionDeclarations");
    ASSERT_EQ(declarations.size(), shared.at("tools").size());
    for (std::size_t i = 0; i < declarations.size(); ++i) {
        EXPECT_EQ(declarations[i].at("name"), shared["tools"][i].at("name"));
        EXPECT_EQ(declarations[i].at("parametersJsonSchema"), shared["tools"][i].at("parameters"));
        EXPECT_FALSE(declarations[i].contains("strict"));
    }
    request.include_tool_instructions = false;
    request.maintenance_tool = {};
    const auto auxiliary = Json::parse(build_google_request_body(request, config, "Auxiliary"));
    EXPECT_EQ(auxiliary["systemInstruction"]["parts"][0]["text"], "Auxiliary");
    EXPECT_FALSE(auxiliary.contains("tools"));
}

TEST(GoogleApi, WebToolInstructionsRespectNoneMode) {
    Json body;
    add_web_search_tool(body, ProviderApi::google);
    add_web_read_tool(body, ProviderApi::google);
    update_tool_instructions(body, ProviderApi::google);
    EXPECT_NE(body["systemInstruction"].dump().find("Web search is available"), std::string::npos);
    body["toolConfig"]["functionCallingConfig"]["mode"] = "NONE";
    update_tool_instructions(body, ProviderApi::google);
    EXPECT_NE(body["systemInstruction"].dump().find("Web search is unavailable"), std::string::npos);
    EXPECT_EQ(body["tools"][0]["functionDeclarations"].size(), 2u);
    EXPECT_FALSE(body.contains("messages"));
    EXPECT_FALSE(body.contains("tool_choice"));
}

TEST(GoogleApi, FullJsonAndEverySseSplitPreservePartsAndLatestUsage) {
    const Json parts = Json::array({
        {{"text", "Private"}, {"thought", true}, {"thoughtSignature", "reasoning-signature"}},
        {{"text", "Checking."}},
        {{"functionCall", {{"id", "supplied"}, {"name", "web_search"}, {"args", {{"query", "first"}}}}},
            {"thoughtSignature", "call-signature"}},
        {{"functionCall", {{"name", "web_read"}, {"args", {{"url", "https://example.test"}}}}}},
        {{"thoughtSignature", "tail-signature"}},
    });
    auto full = response(parts);
    full["usageMetadata"] = {{"promptTokenCount", 10}, {"candidatesTokenCount", 5},
        {"thoughtsTokenCount", 3}, {"cachedContentTokenCount", 4}, {"totalTokenCount", 999}};
    Output complete;
    const auto expected = decode_google_response(full.dump(), complete.sink, true);
    ASSERT_EQ(expected.outcome, GenerationOutcome::completed);
    ASSERT_EQ(expected.tool_calls.size(), 2u);
    EXPECT_EQ(expected.tool_calls[0].id, "supplied");
    EXPECT_EQ(expected.tool_calls[1].id, "google_call_1");
    EXPECT_EQ(Json::parse(expected.tool_calls[0].arguments), (Json{{"query", "first"}}));
    EXPECT_EQ(Json::parse(expected.continuation).at("parts"), parts);
    EXPECT_EQ(complete.answer, "Checking.");
    EXPECT_EQ(complete.reasoning, "Private");
    std::string stream;
    for (const auto& part : parts) {
        auto chunk = response(Json::array({part}), "");
        if (part.contains("functionCall")) chunk["candidates"][0]["finishReason"] = "STOP";
        stream += "data: " + chunk.dump() + "\n\n";
    }
    stream += "data: {\"usageMetadata\":{\"promptTokenCount\":9,\"candidatesTokenCount\":2,\"thoughtsTokenCount\":3}}\n\n";
    stream += "data: " + Json{{"usageMetadata", {{"promptTokenCount", 10}, {"candidatesTokenCount", 5}, {"cachedContentTokenCount", 4}}}}.dump();
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        Output output;
        GoogleStreamDecoder decoder(output.sink, true);
        decoder.consume(std::string_view(stream).substr(0, split));
        decoder.consume(std::string_view(stream).substr(split));
        const auto result = decoder.finish().result;
        ASSERT_EQ(result.outcome, expected.outcome) << result.message << " split=" << split;
        EXPECT_EQ(result.continuation, expected.continuation);
        EXPECT_EQ(result.usage.input_tokens, 10u);
        EXPECT_EQ(result.usage.output_tokens, 8u);
        EXPECT_EQ(result.usage.cache_read_tokens, 4u);
        EXPECT_FALSE(result.usage.cache_write_tokens);
        EXPECT_EQ(output.answer, complete.answer);
        EXPECT_EQ(output.reasoning, complete.reasoning);
    }
}


TEST(GoogleApi, TerminalContentCanOmitEmptyParts) {
    for (const auto* reason : {"STOP", "MAX_TOKENS"}) {
        const auto terminal = Json{{"candidates", Json::array({{
            {"content", {{"role", "model"}}}, {"finishReason", reason}}})},
            {"usageMetadata", {{"candidatesTokenCount", 2}}}};
        Output output;
        GoogleStreamDecoder decoder(output.sink);
        decoder.consume("data: " + response(Json::array({{{"text", "Delivered"}}}), "").dump()
            + "\n\ndata: " + terminal.dump() + "\n\n");
        const auto streamed = decoder.finish().result;
        EXPECT_EQ(streamed.outcome, GenerationOutcome::completed) << streamed.message;
        EXPECT_EQ(streamed.finish_reason, std::string_view(reason) == "STOP" ? "stop" : "length");
        EXPECT_EQ(streamed.usage.output_tokens, 2u);
        EXPECT_EQ(output.answer, "Delivered");
        const auto full = decode_google_response(terminal.dump(), output.sink);
        EXPECT_EQ(full.outcome, GenerationOutcome::protocol_error);
        EXPECT_EQ(full.message, "Response completed without answer content");
        EXPECT_EQ(full.finish_reason, streamed.finish_reason);
    }
}

TEST(GoogleApi, MissingUsageStaysUnknownAndMissingThoughtsContributeZero) {
    for (const auto& counts : {Json::object(), Json{{"candidatesTokenCount", 0}},
             Json{{"candidatesTokenCount", 7}}, Json{{"thoughtsTokenCount", 8}}}) {
        auto value = response(Json::array({{{"text", "Answer"}}}));
        value["usageMetadata"] = counts;
        Output output;
        const auto result = decode_google_response(value.dump(), output.sink);
        EXPECT_EQ(result.outcome, GenerationOutcome::completed);
        EXPECT_EQ(result.usage.output_tokens.has_value(), counts.contains("candidatesTokenCount"));
        if (counts.contains("candidatesTokenCount")) EXPECT_EQ(result.usage.output_tokens, counts["candidatesTokenCount"].get<unsigned>());
        EXPECT_FALSE(result.usage.input_tokens);
        EXPECT_FALSE(result.usage.cache_read_tokens);
    }
}

TEST(GoogleApi, TerminalReasonsPreserveTextAndRejectUnsafeToolCalls) {
    for (const auto* reason : {"STOP", "MAX_TOKENS", "SAFETY", "RECITATION", "BLOCKLIST",
             "PROHIBITED_CONTENT", "SPII", "MALFORMED_FUNCTION_CALL", "OTHER", ""}) {
        for (const bool calls : {false, true}) {
            SCOPED_TRACE(std::string(reason) + (calls ? " calls" : " text"));
            auto parts = Json::array({{{"text", "Partial"}}});
            if (calls) parts.push_back({{"functionCall", {{"name", "assistant_events"}, {"args", Json::object()}}}});
            const bool stop = std::string_view(reason) == "STOP";
            const bool length = std::string_view(reason) == "MAX_TOKENS";
            const bool refused = std::string_view(reason) == "SAFETY" || std::string_view(reason) == "RECITATION"
                || std::string_view(reason) == "BLOCKLIST" || std::string_view(reason) == "PROHIBITED_CONTENT" || std::string_view(reason) == "SPII";
            for (const bool stream : {false, true}) {
                Output output;
                GenerationResult result;
                const auto body = response(parts, reason).dump();
                if (stream) {
                    GoogleStreamDecoder decoder(output.sink, true, maintenance_argument_limit, true);
                    for (const auto byte : "data: " + body + "\n\n") decoder.consume(std::string_view(&byte, 1));
                    result = decoder.finish().result;
                } else result = decode_google_response(body, output.sink, true, maintenance_argument_limit, true);
                EXPECT_EQ(output.answer, "Partial");
                EXPECT_EQ(result.refused, refused);
                EXPECT_EQ(result.outcome == GenerationOutcome::completed, stop || (!calls && (length || refused)));
                if (!stop) EXPECT_TRUE(result.tool_calls.empty());
                if (length) EXPECT_EQ(result.finish_reason, "length");
                if (refused) EXPECT_EQ(result.finish_reason, "content_filter");
            }
        }
    }
}


TEST(GoogleApi, MaintenanceUsesTheSharedArgumentLimit) {
    const auto body = response(Json::array({{{"functionCall", {{"name", "vault_config_apply"},
        {"args", {{"content", std::string(ordinary_tool_argument_limit + 1, 'a')}}}}}}})).dump();
    Output output;
    const auto ordinary = decode_google_response(body, output.sink, true);
    EXPECT_EQ(ordinary.outcome, GenerationOutcome::protocol_error);
    EXPECT_EQ(ordinary.message, "Tool arguments too large");
    const auto maintenance = decode_google_response(body, output.sink, true, maintenance_argument_limit, true);
    EXPECT_EQ(maintenance.outcome, GenerationOutcome::completed);
    ASSERT_EQ(maintenance.tool_calls.size(), 1u);
    const auto oversized = response(Json::array({{{"functionCall", {{"name", "vault_config_apply"},
        {"args", {{"content", std::string(maintenance_argument_limit, 'a')}}}}}}})).dump();
    const auto rejected = decode_google_response(oversized, output.sink, true, maintenance_argument_limit, true);
    EXPECT_EQ(rejected.outcome, GenerationOutcome::protocol_error);
    EXPECT_TRUE(rejected.tool_calls.empty());
    auto truncated = Json::parse(oversized);
    truncated["candidates"][0]["finishReason"] = "MAX_TOKENS";
    const auto limited = decode_google_response(truncated.dump(), output.sink, true, maintenance_argument_limit, true);
    EXPECT_EQ(limited.outcome, GenerationOutcome::protocol_error);
    EXPECT_EQ(limited.message, maintenance_output_limit_message);
    EXPECT_TRUE(limited.tool_calls.empty());
}

TEST(GoogleApi, RejectsBlocksMalformedResponsesAndOversizedArguments) {
    const auto call = Json{{"functionCall", {{"name", "assistant_events"}, {"args", {{"large", std::string(100, 'x')}}}}}};
    for (const auto& body : {std::string("bad json"), std::string("[]"), std::string("{}"),
             response(Json::array()).dump(), response(Json::array({call})).dump(),
             response(Json::array({{{"functionCall", {{"name", ""}}}}})).dump(),
             response(Json::array({{{"functionCall", {{"name", "test"}, {"args", "invalid"}}}}})).dump()}) {
        Output output;
        EXPECT_EQ(decode_google_response(body, output.sink, true, 50).outcome, GenerationOutcome::protocol_error) << body;
    }
    Output output;
    const auto blocked = decode_google_response(R"({"promptFeedback":{"blockReason":"SAFETY"}})", output.sink, true);
    EXPECT_EQ(blocked.outcome, GenerationOutcome::protocol_error);
    EXPECT_TRUE(blocked.refused);
    const auto error = decode_google_response(R"({"error":{"message":"Google diagnostic"}})", output.sink);
    EXPECT_EQ(error.message, "Google diagnostic");
    for (const auto& error_body : {R"({"error":{}})", R"({"error":{"message":""}})"}) {
        const auto generic = decode_google_response(error_body, output.sink);
        EXPECT_EQ(generic.outcome, GenerationOutcome::protocol_error);
        EXPECT_EQ(generic.message, "Google returned an error");
    }
    GoogleStreamDecoder decoder(output.sink, true);
    decoder.consume("data: " + response(Json::array({{{"text", "Delivered"}}})).dump() + "\n\ndata: {bad}\n\n");
    EXPECT_EQ(decoder.finish().result.outcome, GenerationOutcome::protocol_error);
    EXPECT_EQ(output.answer, "Delivered");
}

TEST(GoogleApi, Gemini25AliasesUseFileSearchWithoutFunctions) {
    for (const auto model : {"gemini-2.5-flash-preview-09-2025", "gemini-2.5-pro-latest",
            "models/gemini-2.5-flash-lite-preview"}) EXPECT_TRUE(google_file_search_only_model(model));
    EXPECT_FALSE(google_file_search_only_model("gemini-3-flash"));
}

TEST(GoogleApi, FolderSearchCapabilitiesAndDeclarationOrderAreIndependent) {
    for (const auto model : {"gemini-2.5-pro", "gemini-2.5-flash", "models/gemini-2.5-flash-lite", "gemini-3.8-flash", "gemini-4-argon", "unknown-model"}) {
        GenerationRequest request;
        request.history = std::make_shared<const ModelHistory>();
        request.run.prompt_text = "Question";
        request.folder_search = FolderSearchAccess{FolderStoreService::google, {"fileSearchStores/one"}};
        request.web_search_tool = [](auto, const auto&) { return "[]"; };
        request.web_read_tool = [](auto, const auto&, const auto&) { return "page"; };
        ModelBackendConfig config;
        config.model = model;
        const bool only = google_file_search_only_model(model);
        auto body = Json::parse(build_google_request_body(request, config, "System"));
        EXPECT_EQ(body["tools"].size(), only ? 1u : 2u);
        EXPECT_EQ(body["tools"].back(), (Json{{"fileSearch", {{"fileSearchStoreNames", {"fileSearchStores/one"}}}}}));
        if (!only) {
            EXPECT_EQ(body["toolConfig"], (Json{{"includeServerSideToolInvocations", true},
                {"functionCallingConfig", {{"mode", "VALIDATED"}}}}));
            EXPECT_EQ(body["tools"][0]["functionDeclarations"].size(), 2u);
        } else EXPECT_FALSE(body.contains("toolConfig"));
        EXPECT_NE(body["systemInstruction"].dump().find(only ? "Web search is unavailable" : "Web search is available"), std::string::npos);
        EXPECT_NE(body["systemInstruction"].dump().find("Folder search is available"), std::string::npos);
        request.maintenance_tool = [](auto, auto, const auto&) { return "{}"; };
        body = Json::parse(build_google_request_body(request, config, "System"));
        for (const auto& tool : body["tools"]) EXPECT_FALSE(tool.contains("fileSearch"));
    }
    for (const bool search_first : {false, true}) {
        Json body{{"contents", Json::array()}};
        const Json search{{"fileSearch", {{"fileSearchStoreNames", {"fileSearchStores/one"}}}}};
        if (search_first) body["tools"] = Json::array({search});
        add_web_search_tool(body, ProviderApi::google);
        if (!search_first) body["tools"].push_back(search);
        add_web_read_tool(body, ProviderApi::google);
        ASSERT_EQ(body["tools"].size(), 2u);
        EXPECT_EQ(body["tools"][search_first ? 1 : 0]["functionDeclarations"].size(), 2u);
        body["toolConfig"] = {{"includeServerSideToolInvocations", true}, {"functionCallingConfig", {{"mode", "NONE"}}}};
        update_tool_instructions(body, ProviderApi::google);
        const auto instruction = body["systemInstruction"].dump();
        EXPECT_NE(instruction.find("Web search is unavailable"), std::string::npos);
        EXPECT_NE(instruction.find("Web page reading is unavailable"), std::string::npos);
        EXPECT_NE(instruction.find("Folder search is available"), std::string::npos);
    }
}

} // namespace
} // namespace cha
