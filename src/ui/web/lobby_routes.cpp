#include "ui/web/lobby_routes.h"

#include "application/builtins.h"
#include "application/web_discovery.h"
#include "application/welcome_storage.h"
#include "session/workspace.h"
#include "ui/web/http_response.h"
#include "ui/web/json.h"
#include "ui/web/protocol.h"
#include "ui/web/route_support.h"
#include "ui/web/session_registry.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cha::web {
namespace {

bool is_running(const RegistrySnapshot& snapshot, const SessionIdentity& key) {
    return std::find(snapshot.running_sessions.begin(), snapshot.running_sessions.end(), key)
        != snapshot.running_sessions.end();
}

void set_open_result(
    httplib::Response& response,
    const SessionIdentity& identity,
    const RegistryOpenResult& result) {
    if (std::holds_alternative<RegistryReady>(result)) {
        set_json_response(response, 200, nlohmann::json(OpenSessionSuccess{
            identity.forum_id, identity.session_id}));
        return;
    }
    switch (std::get<RegistryOpenFailure>(result)) {
    case RegistryOpenFailure::not_found:
        set_route_not_found(response);
        return;
    case RegistryOpenFailure::busy:
        set_error_response(response, 409, {ErrorCode::session_busy, "Session is busy."});
        return;
    case RegistryOpenFailure::stopping:
        set_error_response(response, 409, {ErrorCode::session_stopping, "Session is stopping."});
        return;
    case RegistryOpenFailure::limit_reached:
        set_error_response(response, 503, {ErrorCode::session_limit_reached, "Session limit reached."});
        return;
    case RegistryOpenFailure::open_timeout:
        set_error_response(response, 503, {ErrorCode::session_open_timeout, "Session is still opening."});
        return;
    case RegistryOpenFailure::registry_stopping:
        set_error_response(response, 503, {ErrorCode::server_stopping, "Server is stopping."});
        return;
    case RegistryOpenFailure::internal_error:
        set_error_response(response, 500, {ErrorCode::internal_error, "Session could not be opened."});
        return;
    }
}

CharacterSummary character_summary(const CharacterDefinitionMetadata& character) {
    return {character.id, character.display_name, character.description};
}

ForumSummary forum_summary(const Forum& forum, const WebDiscovery& discovery) {
    ForumSummary result{.id = forum.name, .display_name = forum.display_name,
                        .default_character_id = forum.default_agent_id};
    result.members.reserve(forum.character_names.size());
    for (const std::string& id : forum.character_names) {
        const CharacterDefinitionMetadata* character = discovery.find_character(id);
        if (character == nullptr) throw std::runtime_error("Forum member is absent from discovery");
        result.members.push_back(character_summary(*character));
    }
    std::sort(result.members.begin(), result.members.end(), [](const CharacterSummary& left, const CharacterSummary& right) {
        return left.display_name < right.display_name;
    });
    return result;
}

std::int64_t updated_at(const std::filesystem::path& path) {
    const auto time = std::filesystem::last_write_time(path);
    const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count();
}

std::vector<SessionListing> sessions_for(
    const Workspace& workspace, const WebDiscovery& discovery, const WelcomeStorage& welcome_storage,
    const RegistrySnapshot& snapshot,
    std::string_view forum_id) {
    if (forum_id == entrance_id) {
        return {{std::string(welcome_id), std::string(welcome_name),
                 is_running(snapshot, {std::string(entrance_id), std::string(welcome_id)}),
                 updated_at(welcome_storage.database_path())}};
    }
    if (discovery.find_forum(forum_id) == nullptr) throw ForumNotFoundError("Forum not found");
    std::vector<SessionListing> result;
    for (const SessionSummary& stored : workspace.sessions(std::string(forum_id))) {
        result.push_back({stored.id, stored.label, is_running(snapshot, {std::string(forum_id), stored.id}),
                          updated_at(workspace.root() / "forums" / std::string(forum_id) / "sessions" / (stored.id + ".sqlite3"))});
    }
    return result;
}

} // namespace

LobbyRoutes::LobbyRoutes(
    std::shared_ptr<const Workspace> workspace,
    const WebDiscovery& discovery,
    const WelcomeStorage& welcome_storage,
    SessionRegistry& registry,
    WebSettings settings)
    : workspace_(std::move(workspace)), discovery_(discovery), welcome_storage_(welcome_storage), registry_(registry), settings_(std::move(settings)) {
    if (!workspace_) throw std::invalid_argument("Lobby routes need a workspace");
}

void LobbyRoutes::install(httplib::Server& server) const {
    const auto workspace = workspace_;
    const WebDiscovery* const discovery = &discovery_;
    const WelcomeStorage* const welcome_storage = &welcome_storage_;
    SessionRegistry* const registry = &registry_;
    const WebSettings settings = settings_;
    server.Get("/health", [registry](const httplib::Request&, httplib::Response& response) {
        const RegistrySnapshot snapshot = registry->snapshot();
        set_json_response(response, 200, nlohmann::json{
            {"ready", true}, {"live_session_count", snapshot.live_entry_count}});
    });

    server.Get("/api/v1/bootstrap", [workspace, discovery, welcome_storage, registry](const httplib::Request&, httplib::Response& response) {
        Bootstrap bootstrap{.initial_persona_id = std::string(guest_id), .initial_forum_id = std::string(entrance_id), .initial_session_id = std::string(welcome_id)};
        for (const Persona& persona : discovery->personas()) bootstrap.personas.push_back({persona.id, persona.display_name, persona.description});
        for (const CharacterDefinitionMetadata& character : discovery->characters()) bootstrap.characters.push_back(character_summary(character));
        for (const Forum& forum : discovery->forums()) bootstrap.forums.push_back(forum_summary(forum, *discovery));
        const RegistrySnapshot snapshot = registry->snapshot();
        for (const Forum& forum : discovery->forums()) {
            for (const SessionListing& session : sessions_for(*workspace, *discovery, *welcome_storage, snapshot, forum.name)) {
                bootstrap.recent_sessions.push_back({forum.name, session.id, session.label, session.updated_at});
            }
        }
        std::sort(bootstrap.recent_sessions.begin(), bootstrap.recent_sessions.end(), [](const RecentSession& left, const RecentSession& right) { return left.updated_at > right.updated_at; });
        set_json_response(response, 200, nlohmann::json(bootstrap));
    });

    server.Get(R"(/api/v1/characters/([^/]+))", [workspace, discovery](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) return set_route_not_found(response);
        const CharacterDefinitionMetadata* character = discovery->find_character(id);
        if (character == nullptr) return set_route_not_found(response);
        if (id == assistant_id) return set_json_response(response, 200, nlohmann::json(CharacterDetail{character_summary(*character), std::string(application_guide())}));
        const std::filesystem::path path = workspace->root() / "characters" / id / "CHARACTER.md";
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Character definition is unreadable");
        const std::string markdown{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        set_json_response(response, 200, nlohmann::json(CharacterDetail{character_summary(*character), markdown}));
    });

    server.Get(R"(/api/v1/forums/([^/]+)/sessions)", [workspace, discovery, welcome_storage, registry](const httplib::Request& request, httplib::Response& response) {
        const std::string forum = request.matches[1];
        if (!is_valid_route_component(forum)) return set_route_not_found(response);
        try {
            const RegistrySnapshot snapshot = registry->snapshot();
            set_json_response(response, 200, nlohmann::json(sessions_for(*workspace, *discovery, *welcome_storage, snapshot, forum)));
        } catch (const ForumNotFoundError&) {
            set_route_not_found(response);
        }
    });

    server.Post(R"(/api/v1/forums/([^/]+)/sessions)", [workspace, settings](const httplib::Request& request, httplib::Response& response) {
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
            const SessionSummary created = workspace->create_stored_session(forum, std::move(label));
            set_json_response(response, 201, nlohmann::json(CreateSessionSuccess{created.id, created.label}));
        } catch (const ForumNotFoundError&) {
            set_route_not_found(response);
        }
    });

    server.Post(R"(/api/v1/forums/([^/]+)/sessions/([^/]+)/open)", [workspace, registry, settings](const httplib::Request& request, httplib::Response& response) {
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
        if (const auto reattached = registry->try_reattach(key)) {
            return set_open_result(response, key, *reattached);
        }
        if (key.forum_id != entrance_id) {
            try {
                workspace->check_session(key.forum_id, key.session_id);
            } catch (const ForumNotFoundError&) {
                return set_route_not_found(response);
            } catch (const SessionNotFoundError&) {
                return set_route_not_found(response);
            }
        }
        set_open_result(
            response,
            key, registry->open(key, settings.open_deadline));
    });
}

} // namespace cha::web
