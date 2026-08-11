#include "web/lobby_routes.h"

#include "workspace/workspace_definition.h"
#include "session/not_found_error.h"
#include "session/session_repository.h"
#include "web/http_response.h"
#include "web/json.h"
#include "web/protocol.h"
#include "web/route_support.h"
#include "web/live_session_manager.h"
#include "util/text.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cha::web {
namespace {

bool is_running(const LiveSessionManagerSnapshot& snapshot, const SessionIdentity& key) {
    return std::find(snapshot.running_sessions.begin(), snapshot.running_sessions.end(), key)
        != snapshot.running_sessions.end();
}

void set_open_result(
    httplib::Response& response,
    const SessionIdentity& identity,
    const LiveSessionOpenResult& result) {
    if (std::holds_alternative<LiveSessionReady>(result)) {
        set_json_response(response, 200, nlohmann::json(OpenSessionSuccess{
            identity.forum_id, identity.session_id}));
        return;
    }
    switch (std::get<LiveSessionOpenFailure>(result)) {
    case LiveSessionOpenFailure::not_found:
        set_route_not_found(response);
        return;
    case LiveSessionOpenFailure::busy:
        set_error_response(response, 409, {ErrorCode::session_busy, "Session is busy."});
        return;
    case LiveSessionOpenFailure::stopping:
        set_error_response(response, 409, {ErrorCode::session_stopping, "Session is stopping."});
        return;
    case LiveSessionOpenFailure::limit_reached:
        set_error_response(response, 503, {ErrorCode::session_limit_reached, "Session limit reached."});
        return;
    case LiveSessionOpenFailure::open_timeout:
        set_error_response(response, 503, {ErrorCode::session_open_timeout, "Session is still opening."});
        return;
    case LiveSessionOpenFailure::manager_stopping:
        set_error_response(response, 503, {ErrorCode::server_stopping, "Server is stopping."});
        return;
    case LiveSessionOpenFailure::internal_error:
        set_error_response(response, 500, {ErrorCode::internal_error, "Session could not be opened."});
        return;
    }
}

CharacterSummary character_summary(const CharacterMetadata& character) {
    return {character.id, character.display_name, character.description, character.appearance};
}

ForumSummary forum_summary(const ForumInfo& forum, const WorkspaceDefinition& model) {
    const Persona* const persona = model.find_persona(forum.default_persona_id);
    if (persona == nullptr) throw std::runtime_error("Forum default persona is absent from the workspace model");
    ForumSummary result{.id = forum.id, .display_name = forum.display_name,
                        .default_character_id = forum.default_character_id,
                        .default_persona_id = forum.default_persona_id,
                        .default_persona_display_name = persona->display_name};
    result.members.reserve(forum.member_ids.size());
    for (const std::string& id : forum.member_ids) {
        const CharacterMetadata* character = model.find_character(id);
        if (character == nullptr) throw std::runtime_error("Forum member is absent from the workspace model");
        result.members.push_back(character_summary(*character));
    }
    std::sort(result.members.begin(), result.members.end(), [](const CharacterSummary& left, const CharacterSummary& right) {
        return fold_ascii(left.display_name) < fold_ascii(right.display_name);
    });
    return result;
}

std::int64_t updated_at(std::filesystem::file_time_type time) {
    const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count();
}

std::vector<SessionListing> sessions_for(
    const SessionRepository& sessions,
    const LiveSessionManagerSnapshot& snapshot,
    std::string_view forum_id) {
    std::vector<SessionListing> result;
    for (const StoredSession& stored : sessions.list(forum_id)) {
        result.push_back({stored.identity.session_id, stored.label,
                          is_running(snapshot, stored.identity),
                          updated_at(stored.updated_at)});
    }
    return result;
}

std::vector<RecentSession> recent_sessions(
    const WorkspaceDefinition& model,
    const SessionRepository& sessions) {
    std::vector<RecentSession> result;
    for (const ForumInfo& forum : model.forums()) {
        for (const StoredSession& stored : sessions.list(forum.id)) {
            result.push_back({forum.id, stored.identity.session_id, stored.label,
                updated_at(stored.updated_at)});
        }
    }
    std::sort(result.begin(), result.end(),
        [](const RecentSession& left, const RecentSession& right) {
            return left.updated_at > right.updated_at;
        });
    return result;
}

} // namespace

LobbyRoutes::LobbyRoutes(
    std::shared_ptr<const WorkspaceDefinition> model,
    std::shared_ptr<const SessionRepository> sessions,
    InitialSelection initial,
    LiveSessionManager& live_sessions,
    WebSettings settings)
    : model_(std::move(model)), sessions_(std::move(sessions)),
      initial_(std::move(initial)), live_sessions_(live_sessions),
      settings_(std::move(settings)) {
    if (!model_) throw std::invalid_argument("Lobby routes need a workspace model");
    if (!sessions_) throw std::invalid_argument("Lobby routes need a session repository");
}

void LobbyRoutes::install(httplib::Server& server) const {
    const auto model = model_;
    const auto sessions = sessions_;
    const InitialSelection initial = initial_;
    LiveSessionManager* const live_sessions = &live_sessions_;
    const WebSettings settings = settings_;
    server.Get("/health", [live_sessions](const httplib::Request&, httplib::Response& response) {
        const LiveSessionManagerSnapshot snapshot = live_sessions->snapshot();
        set_json_response(response, 200, nlohmann::json{
            {"ready", true}, {"live_session_count", snapshot.live_session_count}});
    });

    server.Get("/api/v1/bootstrap", [model, sessions, initial](const httplib::Request&, httplib::Response& response) {
        Bootstrap bootstrap{.initial_forum_id = initial.session.forum_id,
                            .initial_session_id = initial.session.session_id};
        for (const Persona& persona : *model->personas()) {
            bootstrap.personas.push_back({persona.id, persona.display_name, persona.description});
        }
        for (const CharacterMetadata& character : model->characters()) bootstrap.characters.push_back(character_summary(character));
        for (const ForumInfo& forum : model->forums()) bootstrap.forums.push_back(forum_summary(forum, *model));
        bootstrap.recent_sessions = recent_sessions(*model, *sessions);
        set_json_response(response, 200, nlohmann::json(bootstrap));
    });

    server.Get(R"(/api/v1/characters/([^/]+))", [model](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) return set_route_not_found(response);
        const CharacterMetadata* character = model->find_character(id);
        if (character == nullptr) return set_route_not_found(response);
        set_json_response(response, 200, nlohmann::json(CharacterDetail{
            character_summary(*character),
            std::string(model->character_markdown(id))}));
    });

    server.Get(R"(/api/v1/personas/([^/]+))", [model](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) return set_route_not_found(response);
        const Persona* const persona = model->find_persona(id);
        if (persona == nullptr) return set_route_not_found(response);
        set_json_response(response, 200, nlohmann::json(PersonaDetail{
            {persona->id, persona->display_name, persona->description}, persona->prompt}));
    });

    server.Get(R"(/api/v1/forums/([^/]+)/sessions)", [sessions, live_sessions](const httplib::Request& request, httplib::Response& response) {
        const std::string forum = request.matches[1];
        if (!is_valid_route_component(forum)) return set_route_not_found(response);
        try {
            const LiveSessionManagerSnapshot snapshot = live_sessions->snapshot();
            set_json_response(response, 200, nlohmann::json(sessions_for(*sessions, snapshot, forum)));
        } catch (const ForumNotFoundError&) {
            set_route_not_found(response);
        }
    });

    server.Post(R"(/api/v1/forums/([^/]+)/sessions)", [sessions, settings](const httplib::Request& request, httplib::Response& response) {
        const std::string forum = request.matches[1];
        if (!is_valid_route_component(forum)) return set_route_not_found(response);
        if (!validate_json_mutation(request, response)) return;
        std::string label;
        if (!parse_route_json_body(
                request,
                response,
                settings.request_body_limit,
                [&label](const nlohmann::json& json) {
                    label = parse_create_session_label(json);
                })) return;
        try {
            const StoredSession created = sessions->create(forum, std::move(label));
            set_json_response(response, 201, nlohmann::json(CreateSessionSuccess{
                created.identity.session_id, created.label}));
        } catch (const ForumNotFoundError&) {
            set_route_not_found(response);
        }
    });

    server.Post(R"(/api/v1/forums/([^/]+)/sessions/([^/]+)/open)", [sessions, live_sessions, settings](const httplib::Request& request, httplib::Response& response) {
        const SessionIdentity key{request.matches[1], request.matches[2]};
        if (!is_valid_route_component(key.forum_id)
            || !is_valid_route_component(key.session_id)) {
            return set_route_not_found(response);
        }
        if (!validate_json_mutation(request, response)) return;
        if (!parse_route_json_body(
                request,
                response,
                settings.request_body_limit,
                [](const nlohmann::json& json) { parse_empty_object(json); })) return;
        if (const auto reattached = live_sessions->try_reattach(key)) {
            return set_open_result(response, key, *reattached);
        }
        try {
            sessions->validate(key);
        } catch (const ForumNotFoundError&) {
            return set_route_not_found(response);
        } catch (const SessionNotFoundError&) {
            return set_route_not_found(response);
        }
        set_open_result(
            response,
            key, live_sessions->open(key, settings.open_deadline));
    });
}

} // namespace cha::web
