#include "web/http_response.h"
#include "web/http_server.h"
#include "web/json.h"
#include "web/protocol.h"
#include "web/web_settings.h"
#include "support/test_workspace.h"

#include "agents/character_config.h"
#include "workspace/workspace_definition.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cha::web {
namespace {

// A character that says nothing about its appearance still carries one on the
// wire, so every expectation below names the defaults rather than omitting them.
nlohmann::json default_appearance() {
    return {{"font", "sans"}, {"style", "normal"}, {"weight", "normal"}, {"size", "normal"}};
}

template<typename Enum>
void expect_spellings(
    std::initializer_list<std::pair<Enum, std::string_view>> mappings) {
    for (const auto& [value, spelling] : mappings) {
        EXPECT_EQ(to_string(value), spelling);
    }
}

template<typename Enum>
void expect_invalid_enum_rejected() {
    EXPECT_THROW(
        (void)to_string(static_cast<Enum>(100)),
        std::invalid_argument);
}

void expect_no_transport_location_fields(const nlohmann::json& value) {
    if (value.is_array()) {
        for (const auto& child : value) {
            expect_no_transport_location_fields(child);
        }
        return;
    }
    if (!value.is_object()) {
        return;
    }
    for (const auto& [key, child] : value.items()) {
        EXPECT_NE(key, "host");
        EXPECT_NE(key, "port");
        EXPECT_NE(key, "url");
        EXPECT_NE(key, "path");
        EXPECT_NE(key, "lobby_address");
        expect_no_transport_location_fields(child);
    }
}

TEST(WebProtocol, SerializesSpecifiedSuccessListingAndErrorBodies) {
    EXPECT_EQ(
        nlohmann::json(CreateSessionSuccess{"s1", "Notes"}),
        nlohmann::json({{"id", "s1"}, {"label", "Notes"}}));
    EXPECT_EQ(
        nlohmann::json(OpenSessionSuccess{"forum", "s1"}),
        nlohmann::json({{"forum_id", "forum"}, {"session_id", "s1"}}));
    EXPECT_EQ(
        nlohmann::json(ForumSummary{"forum", "Forum", std::nullopt, "guide", "reader", "Reader", {{"guide", "Guide"}}}),
        nlohmann::json({{"display_name", "Forum"}, {"id", "forum"}, {"default_character_id", "guide"}, {"default_persona_id", "reader"}, {"default_persona_display_name", "Reader"},
            {"members", {{{"id", "guide"}, {"display_name", "Guide"}, {"appearance", default_appearance()}}}}}));
    EXPECT_EQ(
        nlohmann::json(ForumSummary{"forum", "Forum", "A place to talk", "guide", "reader",
            "Reader", {{"guide", "Guide"}}})["description"],
        "A place to talk");
    // The detail flattens its summary and appends FORUM.md, exactly as the
    // character and persona details do with their own Markdown.
    EXPECT_EQ(
        nlohmann::json(ForumDetail{
            {"forum", "Forum", std::nullopt, "guide", "reader", "Reader", {{"guide", "Guide"}}},
            "# House rules"}),
        nlohmann::json({{"display_name", "Forum"}, {"id", "forum"}, {"default_character_id", "guide"}, {"default_persona_id", "reader"}, {"default_persona_display_name", "Reader"},
            {"members", {{{"id", "guide"}, {"display_name", "Guide"}, {"appearance", default_appearance()}}}},
            {"forum_markdown", "# House rules"}}));
    EXPECT_EQ(
        nlohmann::json(PersonaSummary{"reader", "Reader"}),
        nlohmann::json({{"display_name", "Reader"}, {"id", "reader"}}));
    EXPECT_EQ(
        nlohmann::json(SessionListing{"s1", "Notes", true, 12}),
        nlohmann::json({{"id", "s1"}, {"label", "Notes"}, {"live", true}, {"updated_at", 12}}));
    EXPECT_EQ(
        nlohmann::json(CharacterSummary{"guide", "Guide"}),
        nlohmann::json({{"display_name", "Guide"}, {"id", "guide"},
            {"appearance", default_appearance()}}));
    EXPECT_EQ(
        nlohmann::json(CharacterSummary{"seneca", "Seneca", std::nullopt,
            {CharacterFont::serif, CharacterSlant::italic,
             CharacterWeight::bold, CharacterScale::large}})["appearance"],
        nlohmann::json({{"font", "serif"}, {"style", "italic"},
            {"weight", "bold"}, {"size", "large"}}));
    EXPECT_EQ(
        nlohmann::json(CommandResult{.clear_input = true}),
        nlohmann::json({{"clear_input", true}}));
    EXPECT_EQ(
        nlohmann::json(CommandResult{
            .session = {.notice = std::string{}},
            .clear_input = false,
        }),
        nlohmann::json({{"clear_input", false}, {"notice", ""}}));
    EXPECT_EQ(
        nlohmann::json(Error{ErrorCode::body_too_large, "Too large"}),
        nlohmann::json({
            {"error",
             {
                 {"code", "body_too_large"},
                 {"message", "Too large"},
             }},
        }));
}

TEST(WebProtocol, SerializesSnapshotMailboxPayloadAndTargetAwareAppend) {
    SessionSnapshot snapshot{
        .forum = {"forum", "Forum"},
        .session_id = "session",
        .session_label = "Label",
        .characters = {{"guide", "Guide"}},
        .default_character_id = "guide",
        .transcript = {{
            .id = 7,
            .kind = EntryKind::character,
            .participant_id = "guide",
            .display_name = "Guide",
            .text = "<answer>",
            .status = EntryStatus::streaming,
            .request_id = 3,
        }},
        .generation = {
            .active = true,
            .request_id = 3,
            .character_id = "guide",
            .character_display_name = "Guide",
            .phase = ResponsePhase::answering,
            .reasoning_text = "<reasoning>",
        },
        .notice = std::string{"<notice>"},
        .lifecycle = SessionLifecycle::stopping,
        .shutdown_reason = ShutdownReason::session_failed,
    };

    const auto value = nlohmann::json(SnapshotEvent{std::move(snapshot)});
    const nlohmann::json expected = {
        {"default_character_id", "guide"},
        {"forum", {{"display_name", "Forum"}, {"id", "forum"}, {"default_character_id", ""}, {"default_persona_id", ""}, {"default_persona_display_name", ""}, {"members", nlohmann::json::array()}}},
        {"generation",
         {
             {"active", true},
             {"character_id", "guide"},
             {"character_display_name", "Guide"},
             {"phase", "answering"},
             {"reasoning_text", "<reasoning>"},
             {"request_id", 3},
         }},
        {"lifecycle", "stopping"},
        {"notice", "<notice>"},
        {"characters", {{{"display_name", "Guide"}, {"id", "guide"},
            {"appearance", default_appearance()}}}},
        {"session_id", "session"},
        {"session_label", "Label"},
        {"shutdown_reason", "session_failed"},
        {"transcript",
         {{
             {"addressed_to", ""},
             {"addressed_to_name", ""},
             {"display_name", "Guide"},
             {"id", 7},
             {"kind", "character"},
             {"participant_id", "guide"},
             {"request_id", 3},
             {"status", "streaming"},
             {"text", "<answer>"},
         }}},
    };
    EXPECT_EQ(value, expected);
    expect_no_transport_location_fields(value);

    EXPECT_EQ(
        nlohmann::json(AppendEvent{ReasoningTextTarget{3}, "more", 7}),
        nlohmann::json({
            {"seq", 7},
            {"target", {{"kind", "reasoning"}, {"request_id", 3}}},
            {"text", "more"},
        }));
    EXPECT_EQ(
        nlohmann::json(AppendEvent{EntryTextTarget{7}, "more", 8}),
        nlohmann::json({
            {"seq", 8},
            {"target", {{"entry_id", 7}, {"kind", "entry"}}},
            {"text", "more"},
        }));
}

TEST(WebProtocol, SelectsAppendTargetAndTranscriptIndexTogether) {
    SessionSnapshot snapshot{
        .transcript = {
            {.id = 4, .status = EntryStatus::complete},
            {.id = 7, .status = EntryStatus::streaming},
        },
        .generation = {
            .active = true,
            .request_id = 3,
            .phase = ResponsePhase::reasoning,
        },
    };

    auto selection = snapshot_append_selection(snapshot);
    ASSERT_TRUE(selection);
    EXPECT_EQ(selection->target, TextTarget{EntryTextTarget{7}});
    EXPECT_EQ(selection->transcript_index, 1U);

    snapshot.transcript[1].status = EntryStatus::complete;
    selection = snapshot_append_selection(snapshot);
    ASSERT_TRUE(selection);
    EXPECT_EQ(selection->target, TextTarget{ReasoningTextTarget{3}});
    EXPECT_FALSE(selection->transcript_index);

    snapshot.generation.active = false;
    EXPECT_FALSE(snapshot_append_selection(snapshot));
}

TEST(WebProtocol, EscapesAndOwnsPresentationText) {
    std::string text = "quote \\\" newline\\n";
    TranscriptEntry entry{
        .id = 1,
        .kind = EntryKind::notice,
        .participant_id = "system",
        .display_name = "System",
        .text = text,
    };
    text.assign("changed");
    SessionSnapshot entry_snapshot;
    entry_snapshot.transcript.push_back(std::move(entry));
    const auto value = nlohmann::json(entry_snapshot)["transcript"][0];
    EXPECT_EQ(value["text"], "quote \\\" newline\\n");
    EXPECT_EQ(
        value.dump(),
        "{\"addressed_to\":\"\",\"addressed_to_name\":\"\","
        "\"display_name\":\"System\",\"id\":1,\"kind\":\"notice\","
        "\"participant_id\":\"system\",\"status\":\"complete\","
        "\"text\":\"quote \\\\\\\" newline\\\\n\"}");

    std::string presentation = "quote \\\" newline\\n";
    SessionSnapshot snapshot;
    snapshot.notice = presentation;
    snapshot.generation.reasoning_text = presentation;
    const auto snapshot_value = nlohmann::json(snapshot);
    const auto error_value =
        nlohmann::json(Error{ErrorCode::internal_error, presentation});
    presentation.assign("changed");
    EXPECT_EQ(snapshot_value["notice"], "quote \\\" newline\\n");
    EXPECT_EQ(
        snapshot_value["generation"]["reasoning_text"],
        "quote \\\" newline\\n");
    EXPECT_EQ(error_value["error"]["message"], "quote \\\" newline\\n");
}

TEST(WebProtocol, DefinesEveryEnumSpelling) {
    expect_spellings<EntryKind>({
        {EntryKind::human, "human"},
        {EntryKind::character, "character"},
        {EntryKind::notice, "notice"},
        {EntryKind::error, "error"},
    });
    expect_spellings<EntryStatus>({
        {EntryStatus::complete, "complete"},
        {EntryStatus::streaming, "streaming"},
        {EntryStatus::cancelled, "cancelled"},
        {EntryStatus::failed, "failed"},
    });
    expect_spellings<ResponsePhase>({
        {ResponsePhase::waiting, "waiting"},
        {ResponsePhase::reasoning, "reasoning"},
        {ResponsePhase::answering, "answering"},
        {ResponsePhase::stopping, "stopping"},
    });
    expect_spellings<SessionLifecycle>({
        {SessionLifecycle::starting, "starting"},
        {SessionLifecycle::running, "running"},
        {SessionLifecycle::stopping, "stopping"},
    });
    expect_spellings<ShutdownReason>({
        {ShutdownReason::browser_disconnected, "browser_disconnected"},
        {ShutdownReason::session_failed, "session_failed"},
        {ShutdownReason::session_deleted, "session_deleted"},
        {ShutdownReason::server_stopping, "server_stopping"},
    });
    expect_spellings<ErrorCode>({
        {ErrorCode::not_found, "not_found"},
        {ErrorCode::bad_request, "bad_request"},
        {ErrorCode::body_too_large, "body_too_large"},
        {ErrorCode::prompt_too_large, "prompt_too_large"},
        {ErrorCode::forbidden_origin, "forbidden_origin"},
        {ErrorCode::internal_error, "internal_error"},
        {ErrorCode::session_busy, "session_busy"},
        {ErrorCode::session_stopping, "session_stopping"},
        {ErrorCode::session_limit_reached, "session_limit_reached"},
        {ErrorCode::session_open_timeout, "session_open_timeout"},
        {ErrorCode::server_stopping, "server_stopping"},
        {ErrorCode::session_not_live, "session_not_live"},
        {ErrorCode::browser_stream_in_use, "browser_stream_in_use"},
        {ErrorCode::command_timeout, "command_timeout"},
        {ErrorCode::command_queue_full, "command_queue_full"},
        {ErrorCode::session_delete_conflict, "session_delete_conflict"},
    });
}

TEST(WebProtocol, RejectsInvalidEnumValues) {
    expect_invalid_enum_rejected<EntryKind>();
    expect_invalid_enum_rejected<EntryStatus>();
    expect_invalid_enum_rejected<ResponsePhase>();
    expect_invalid_enum_rejected<SessionLifecycle>();
    expect_invalid_enum_rejected<ShutdownReason>();
    expect_invalid_enum_rejected<ErrorCode>();
}

TEST(WebProtocol, ParsesRouteSpecificCommandPayloads) {
    nlohmann::json input_body = {{"text", "hello"}};
    const WebCommand input = parse_input_command(input_body);
    input_body["text"] = "changed";
    ASSERT_TRUE(std::holds_alternative<RawCommand>(input));
    EXPECT_EQ(std::get<RawCommand>(input).text, "hello");

    const WebCommand default_character =
        parse_default_character_command({{"character_id", "guide"}});
    ASSERT_TRUE(std::holds_alternative<SetDefaultCharacterCommand>(default_character));
    EXPECT_EQ(
        std::get<SetDefaultCharacterCommand>(default_character).character_id,
        "guide");

    const WebCommand stop = StopCommand{};
    EXPECT_TRUE(std::holds_alternative<StopCommand>(stop));
    EXPECT_THROW(
        (void)parse_input_command({{"type", "input"}, {"text", "hello"}}),
        std::invalid_argument);
    EXPECT_THROW(
        (void)parse_input_command({{"text", 1}}),
        std::invalid_argument);
    // Author attribution is the forum's to decide, so naming one is refused
    // rather than quietly ignored.
    EXPECT_THROW(
        (void)parse_input_command({{"persona", "reader"}, {"text", "hello"}}),
        std::invalid_argument);
    EXPECT_THROW(
        (void)parse_default_character_command({
            {"type", "default_character"},
            {"character_id", "guide"},
        }),
        std::invalid_argument);
    EXPECT_THROW(
        (void)parse_default_character_command({}),
        std::invalid_argument);

    EXPECT_EQ(parse_create_session_label({{"label", "Notes"}}), "Notes");
    EXPECT_EQ(parse_rename_session_label({{"label", "Renamed"}}), "Renamed");
    EXPECT_EQ(parse_rename_session_label({{"label", ""}}), "");
    EXPECT_THROW(
        (void)parse_create_session_label({}),
        std::invalid_argument);
    EXPECT_THROW(
        (void)parse_create_session_label({{"label", "Notes"}, {"extra", true}}),
        std::invalid_argument);

    EXPECT_NO_THROW(parse_empty_object(nlohmann::json::object()));
    EXPECT_THROW(parse_empty_object(nullptr), std::invalid_argument);
    EXPECT_THROW(
        parse_empty_object({{"unexpected", true}}),
        std::invalid_argument);
}

TEST(WebProtocol, ParsesBodiesAndBuildsJsonResponses) {
    EXPECT_TRUE(is_json_content_type("Application/JSON; charset=utf-8"));
    EXPECT_TRUE(is_json_content_type(" application/json ; charset=utf-8"));
    EXPECT_FALSE(is_json_content_type("text/plain"));
    EXPECT_EQ(parse_json_body("{}", 2), nlohmann::json::object());
    EXPECT_THROW((void)parse_json_body("{}", 1), std::length_error);
    EXPECT_THROW((void)parse_json_body("{", 1), std::invalid_argument);

    httplib::Response response;
    const Error error{ErrorCode::body_too_large, "Too large"};
    set_error_response(response, 413, error);
    EXPECT_EQ(response.status, 413);
    EXPECT_EQ(response.get_header_value("Content-Type"), "application/json");
    EXPECT_EQ(response.get_header_value("Cache-Control"), "no-store");
    EXPECT_EQ(
        nlohmann::json::parse(response.body),
        nlohmann::json(Error{ErrorCode::body_too_large, "Too large"}));
}

TEST(WebSettings, DefaultsRespectCoupledResourceAndLifetimeLimits) {
    const WebSettings settings;
    EXPECT_GT(settings.http_thread_pool_size, settings.session_limit);
    EXPECT_GT(settings.command_batch_size, 0U);
    EXPECT_GT(settings.event_batch_size, 0U);
    EXPECT_GE(settings.orphan_limit, settings.idle_grace);
    EXPECT_GT(settings.delete_deadline, settings.sse_drain_deadline);
}

TEST(WebSettings, RequestHeadroomCoversNonStreamingWork) {
    const WebSettings settings;
    EXPECT_GE(
        settings.http_thread_pool_size,
        settings.session_limit + settings.http_request_headroom);
    EXPECT_GE(settings.http_pending_request_limit, settings.http_thread_pool_size);
}

TEST(WebSettings, HttpServerRejectsPoolWithoutSessionHeadroom) {
    httplib::Server server;
    WebSettings settings;
    settings.session_limit = 20;
    EXPECT_THROW(
        configure_http_server(server, settings),
        std::invalid_argument);
}

TEST(WebSettings, HttpServerRejectsPendingLimitBelowPoolSize) {
    httplib::Server server;
    WebSettings settings;
    settings.http_pending_request_limit = settings.http_thread_pool_size - 1;
    EXPECT_THROW(
        configure_http_server(server, settings),
        std::invalid_argument);
}

TEST(WebSettings, RequestHeadroomIsInjectable) {
    httplib::Server server;
    WebSettings settings;
    settings.http_request_headroom = 1;
    settings.http_thread_pool_size = settings.session_limit + 1;
    settings.http_pending_request_limit = settings.http_thread_pool_size;
    EXPECT_NO_THROW(configure_http_server(server, settings));
}

TEST(WebProtocol, TemporaryWorkspaceUsesTheDeterministicTestProvider) {
    test::TestWorkspace fixture;
    const WorkspaceDefinition model = WorkspaceDefinition::load(
        fixture.root(), load_workspace_config(fixture.root()));
    const ForumInfo* const forum = model.find_forum("lobby");
    ASSERT_NE(forum, nullptr);
    EXPECT_EQ(forum->display_name, "The Lobby");
    EXPECT_EQ(forum->member_ids, (std::vector<std::string>{"guide"}));
    const auto config = load_character_config({
        .providers = {load_provider_config(fixture.root() / "workspace.toml"),
            providers_directory(fixture.root())},
        .definition = fixture.root()
            / "characters/guide/character.toml",
        .forum_defaults = fixture.root()
            / "forums/lobby/members/character_defaults.toml",
    }).backend;
    EXPECT_EQ(config.mode, Mode::test);
    EXPECT_EQ(config.model, "fake");
}

} // namespace
} // namespace cha::web
