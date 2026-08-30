#include "web/lobby_routes.h"

#include "workspace/workspace.h"
#include "session/session_snapshot.h"
#include "session/not_found_error.h"
#include "session/session_delete_conflict.h"
#include "session/session_lease.h"
#include "session/session_label.h"
#include "session/session_repository.h"
#include "web/http_response.h"
#include "web/json.h"
#include "web/protocol.h"
#include "web/route_support.h"
#include "web/session_markdown.h"
#include "web/live_session_manager.h"
#include "web/workspace_backup.h"
#include "util/text.h"
#include "util/logging.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cha::web {
namespace {

bool is_running(const LiveSessionManagerSnapshot& snapshot, const FullSessionId& key) {
    return std::find(snapshot.running_sessions.begin(), snapshot.running_sessions.end(), key)
        != snapshot.running_sessions.end();
}

std::string session_event(
    const FullSessionId& identity,
    std::string_view event) {
    return "web session forum_id=" + identity.forum_id
        + " session_id=" + identity.session_id
        + " event=" + std::string(event);
}

bool validate_route_session_label(
    httplib::Response& response,
    std::string_view label,
    bool allow_empty) {
    if (allow_empty && label.empty()) return true;
    try {
        validate_session_label(label);
        return true;
    } catch (const std::invalid_argument&) {
        set_error_response(response, 400,
            {ErrorCode::bad_request, "Invalid session label."});
        return false;
    }
}

void set_open_result(
    httplib::Response& response,
    const FullSessionId& identity,
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

void set_command_error(httplib::Response& response, ErrorCode code) {
    switch (code) {
    case ErrorCode::command_queue_full:
        return set_error_response(response, 503, {code, "Session command queue is full."});
    case ErrorCode::command_timeout:
        return set_error_response(response, 503, {code, "Session command timed out."});
    case ErrorCode::server_stopping:
        return set_error_response(response, 503, {code, "Server is stopping."});
    case ErrorCode::session_not_live:
        return set_error_response(response, 409, {code, "That session is not open."});
    default:
        return set_error_response(response, 500,
            {ErrorCode::internal_error, "The request could not be completed."});
    }
}

CharacterSummary character_summary(const WorkspaceCharacter& character) {
    return {
        character.character.id,
        character.character.display_name,
        character.character.description,
        character.character.appearance,
    };
}

std::shared_ptr<const Workspace> published_workspace() {
    std::shared_ptr<const Workspace> workspace = getws();
    if (!workspace) throw std::runtime_error("Workspace is not loaded");
    return workspace;
}

CharacterDetail character_detail(
    const Workspace& workspace,
    const WorkspaceCharacter& character) {
    CharacterDetail detail{character_summary(character), character.markdown};
    detail.writable = workspace.character_is_writable(character.character.id);
    if (detail.writable) {
        detail.provider = character.provider_id;
        detail.style = character.style_id;
    }
    for (const WorkspaceProvider& provider : workspace.providers()) {
        detail.available_providers.push_back({provider.id, provider.label});
    }
    for (const WorkspaceStyle& style : workspace.styles()) {
        detail.available_styles.push_back(
            {style.id, style.label, style.appearance});
    }
    return detail;
}

std::vector<std::string> forums_affected_by_save(
    const Workspace& workspace,
    std::string_view character_id,
    bool changed) {
    if (!changed) return {};
    std::vector<std::string> result;
    for (const WorkspaceForum& forum : workspace.forums()) {
        if (std::ranges::any_of(
                forum.members,
                [character_id](const WorkspaceForumMember& member) {
                    return member.character_id == character_id;
                })) {
            result.push_back(forum.id);
        }
    }
    return result;
}

ForumSummary forum_summary(
    const WorkspaceForum& forum,
    const Workspace& workspace) {
    const WorkspacePersona* persona =
        workspace.find_persona(forum.default_persona_id);
    if (persona == nullptr) {
        throw std::runtime_error("Forum default persona is absent from the workspace");
    }
    ForumSummary result{
        .id = forum.id,
        .display_name = forum.display_name,
        .description = forum.description,
        .default_character_id = forum.default_character_id,
        .default_persona_id = forum.default_persona_id,
        .default_persona_display_name = persona->display_name,
    };
    result.members.reserve(forum.members.size());
    for (const WorkspaceForumMember& member : forum.members) {
        const WorkspaceCharacter* character =
            workspace.find_character(member.character_id);
        if (character == nullptr) {
            throw std::runtime_error("Forum member is absent from the workspace");
        }
        result.members.push_back(character_summary(*character));
    }
    std::ranges::sort(
        result.members, {},
        [](const CharacterSummary& character) {
            return fold_ascii(character.display_name);
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
    const Workspace& workspace,
    const SessionRepository& sessions) {
    std::vector<RecentSession> result;
    for (const WorkspaceForum& forum : workspace.forums()) {
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

Bootstrap bootstrap_for(
    const Workspace& workspace,
    const SessionRepository& sessions,
    const InitialSelection& initial) {
    Bootstrap bootstrap{.initial_forum_id = initial.session.forum_id,
                        .initial_session_id = initial.session.session_id};
    for (const WorkspacePersona& persona : workspace.personas()) {
        bootstrap.personas.push_back({persona.id, persona.display_name, persona.description});
    }
    for (const WorkspaceCharacter& character : workspace.characters()) {
        bootstrap.characters.push_back(character_summary(character));
    }
    for (const WorkspaceForum& forum : workspace.forums()) {
        bootstrap.forums.push_back(forum_summary(forum, workspace));
    }
    bootstrap.recent_sessions = recent_sessions(workspace, sessions);
    return bootstrap;
}

} // namespace

LobbyRoutes::LobbyRoutes(
    std::shared_ptr<const SessionRepository> sessions,
    InitialSelection initial,
    LiveSessionManager& live_sessions,
    std::filesystem::path backup_dir,
    WebSettings settings)
    : sessions_(std::move(sessions)),
      initial_(std::move(initial)), live_sessions_(live_sessions),
      backup_dir_(std::move(backup_dir)),
      settings_(std::move(settings)) {
    if (!sessions_) throw std::invalid_argument("Lobby routes need a session repository");
}

void LobbyRoutes::install(httplib::Server& server) const {
    const auto sessions = sessions_;
    const InitialSelection initial = initial_;
    LiveSessionManager* const live_sessions = &live_sessions_;
    const std::filesystem::path backup_dir = backup_dir_;
    const WebSettings settings = settings_;
    server.Get("/health", [live_sessions](const httplib::Request&, httplib::Response& response) {
        const LiveSessionManagerSnapshot snapshot = live_sessions->snapshot();
        set_json_response(response, 200, nlohmann::json{
            {"ready", true}, {"live_session_count", snapshot.live_session_count}});
    });

    server.Get("/api/v1/bootstrap", [sessions, initial](const httplib::Request&, httplib::Response& response) {
        const auto current = published_workspace();
        set_json_response(response, 200, nlohmann::json(
            bootstrap_for(*current, *sessions, initial)));
    });

    server.Post("/api/v1/workspace/reload",
        [sessions, initial, live_sessions, backup_dir, settings](
            const httplib::Request& request, httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [](const nlohmann::json& json) { parse_empty_object(json); })) return;
        WorkspaceReloadResult reload =
            live_sessions->reserve_workspace_reload(settings.shutdown_grace);
        if (!std::holds_alternative<WorkspaceReloadReservation>(reload)) {
            return set_error_response(response, 503,
                {ErrorCode::workspace_reload_failed,
                 "Workspace reload failed: active sessions did not close."});
        }
        try {
            const auto current = published_workspace();
            (void)backup_workspace(current->root(), backup_dir);
            Workspace candidate = Workspace::load(current->root());
            bootstrap_sessions_from_sql(candidate);
            loadws(std::move(candidate));
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& error) {
            log_warn("workspace reload failed: " + std::string(error.what()));
            return set_error_response(response, 422,
                {ErrorCode::workspace_reload_failed,
                 "Workspace reload failed: " + std::string(error.what())});
        }
        const auto current = published_workspace();
        set_json_response(response, 200, nlohmann::json(
            bootstrap_for(*current, *sessions, initial)));
    });

    server.Get(R"(/api/v1/characters/([^/]+))", [](const httplib::Request& request, httplib::Response& response) {
        const auto workspace = published_workspace();
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That character was not found.");
        }
        const WorkspaceCharacter* character = workspace->find_character(id);
        if (character == nullptr) {
            return set_route_not_found(response, "That character was not found.");
        }
        set_json_response(response, 200, nlohmann::json(character_detail(*workspace, *character)));
    });

    server.Patch(R"(/api/v1/characters/([^/]+))",
        [live_sessions, settings](const httplib::Request& request,
                                  httplib::Response& response) {
        const auto workspace = published_workspace();
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That character was not found.");
        }
        const WorkspaceCharacter* character = workspace->find_character(id);
        if (character == nullptr) {
            return set_route_not_found(response, "That character was not found.");
        }
        if (!workspace->character_is_writable(id)) {
            return set_route_not_found(response, "That character was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        CharacterSettingsUpdate update;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&update](const nlohmann::json& json) {
                    update = parse_character_settings_update(json);
                })) return;
        const bool changed = update.provider != character->provider_id
            || update.style != character->style_id;
        try {
            const std::optional<std::string_view> style = update.style
                ? std::optional<std::string_view>(*update.style) : std::nullopt;
            if (changed) {
                workspace->write_character_settings(id, update.provider, style);
                loadws(workspace->root());
            }
        } catch (const std::invalid_argument&) {
            return set_error_response(response, 400,
                {ErrorCode::bad_request, "Invalid provider or style."});
        }
        if (changed) {
            request_reload(
                *live_sessions,
                forums_affected_by_save(
                    *workspace, id, changed));
        }
        const auto current = published_workspace();
        const WorkspaceCharacter* updated = current->find_character(id);
        if (updated == nullptr) {
            return set_route_not_found(response, "That character was not found.");
        }
        set_json_response(
            response, 200,
            nlohmann::json(character_detail(*current, *updated)));
    });

    server.Get(R"(/api/v1/personas/([^/]+))", [](const httplib::Request& request, httplib::Response& response) {
        const auto workspace = published_workspace();
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That persona was not found.");
        }
        const WorkspacePersona* const persona = workspace->find_persona(id);
        if (persona == nullptr) {
            return set_route_not_found(response, "That persona was not found.");
        }
        set_json_response(response, 200, nlohmann::json(PersonaDetail{
            {persona->id, persona->display_name, persona->description}, persona->prompt}));
    });

    // `[^/]+` cannot span the separator, so this never shadows the session
    // routes registered below it.
    server.Get(R"(/api/v1/forums/([^/]+))", [](const httplib::Request& request, httplib::Response& response) {
        const auto workspace = published_workspace();
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) return set_route_not_found(response);
        const WorkspaceForum* const forum = workspace->find_forum(id);
        if (forum == nullptr) return set_route_not_found(response);
        set_json_response(response, 200, nlohmann::json(ForumDetail{
            forum_summary(*forum, *workspace),
            forum->prompt_template}));
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
        if (!validate_route_session_label(response, label, true)) return;
        try {
            const StoredSession created = sessions->create(forum, std::move(label));
            set_json_response(response, 201, nlohmann::json(CreateSessionSuccess{
                created.identity.session_id, created.label}));
        } catch (const ForumNotFoundError&) {
            set_route_not_found(response);
        } catch (const std::invalid_argument&) {
            set_error_response(response, 400,
                {ErrorCode::bad_request, "Invalid session label."});
        }
    });

    server.Get(R"(/api/v1/forums/([^/]+)/sessions/([^/]+)/download)",
        [sessions, live_sessions, settings](const httplib::Request& request,
                                             httplib::Response& response) {
        const FullSessionId key{request.matches[1], request.matches[2]};
        if (!is_valid_route_component(key.forum_id)
            || !is_valid_route_component(key.session_id)) {
            return set_route_not_found(response);
        }
        try {
            std::string markdown;
            if (const LiveSessionHandle live = live_sessions->lookup(key)) {
                CommandSubmitResult result = live->snapshot(settings.command_deadline);
                if (const auto* snapshot = std::get_if<SessionSnapshot>(&result)) {
                    markdown = session_markdown(
                        snapshot->session_label, snapshot->transcript);
                } else if (const auto* error = std::get_if<ErrorCode>(&result)) {
                    return set_command_error(response, *error);
                } else {
                    return set_error_response(response, 500,
                        {ErrorCode::internal_error,
                         "The session could not be downloaded."});
                }
            } else {
                PreparedSession prepared = sessions->prepare(key);
                markdown = session_markdown(
                    prepared.label, prepared.restore.entries);
            }
            response.status = 200;
            response.set_header("Cache-Control", "no-store");
            response.set_content(markdown, "text/markdown; charset=utf-8");
        } catch (const ForumNotFoundError&) {
            set_route_not_found(response);
        } catch (const SessionNotFoundError&) {
            set_route_not_found(response);
        } catch (const SessionBusyError&) {
            set_error_response(response, 409,
                {ErrorCode::session_busy, "Session is busy."});
        }
    });

    server.Patch(R"(/api/v1/forums/([^/]+)/sessions/([^/]+))",
        [sessions, initial, live_sessions, settings](const httplib::Request& request,
                                                     httplib::Response& response) {
        const FullSessionId key{request.matches[1], request.matches[2]};
        if (!is_valid_route_component(key.forum_id)
            || !is_valid_route_component(key.session_id)) {
            return set_route_not_found(response);
        }
        // Welcome is process-local and immutable. Reject it before consulting
        // the live registry, which deliberately bypasses repository reads for
        // an already-open session.
        if (key == initial.session) return set_route_not_found(response);
        if (!validate_json_mutation(request, response)) return;
        std::string label;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&label](const nlohmann::json& json) {
                    label = parse_rename_session_label(json);
                })) return;
        if (!validate_route_session_label(response, label, false)) return;
        log_info(session_event(key, "rename_requested"));
        try {
            if (const LiveSessionHandle live = live_sessions->lookup(key)) {
                CommandSubmitResult result = live->submit(
                    RenameSessionCommand{std::move(label)},
                    settings.command_deadline);
                if (const auto* renamed = std::get_if<SessionLabelResult>(&result)) {
                    log_info(session_event(key, "rename_committed"));
                    return set_json_response(response, 200, nlohmann::json(*renamed));
                }
                if (const auto* error = std::get_if<ErrorCode>(&result)) {
                    log_warn(session_event(key, "rename_failed"));
                    return set_command_error(response, *error);
                }
                return set_error_response(response, 500,
                    {ErrorCode::internal_error, "The request could not be completed."});
            }
            const StoredSession renamed = sessions->rename(key, std::move(label));
            log_info(session_event(key, "rename_committed"));
            set_json_response(response, 200, nlohmann::json(SessionLabelResult{
                renamed.identity.session_id, renamed.label}));
        } catch (const ForumNotFoundError&) {
            log_warn(session_event(key, "rename_failed"));
            set_route_not_found(response);
        } catch (const SessionNotFoundError&) {
            log_warn(session_event(key, "rename_failed"));
            set_route_not_found(response);
        } catch (const SessionBusyError&) {
            log_warn(session_event(key, "rename_failed"));
            set_error_response(response, 409,
                {ErrorCode::session_busy, "Session is busy."});
        } catch (const std::invalid_argument&) {
            log_warn(session_event(key, "rename_failed"));
            set_error_response(response, 400,
                {ErrorCode::bad_request, "Invalid session label."});
        } catch (...) {
            log_warn(session_event(key, "rename_failed"));
            throw;
        }
    });

    server.Delete(R"(/api/v1/forums/([^/]+)/sessions/([^/]+))",
        [sessions, initial, live_sessions, settings](const httplib::Request& request,
                                                     httplib::Response& response) {
        const FullSessionId key{request.matches[1], request.matches[2]};
        if (!is_valid_route_component(key.forum_id)
            || !is_valid_route_component(key.session_id)) {
            return set_route_not_found(response);
        }
        // The guard must precede the maintenance reservation: acquiring one
        // may stop a live actor, which is an impermissible side effect for an
        // immutable-session request that will ultimately be rejected.
        if (key == initial.session) return set_route_not_found(response);
        if (!validate_json_mutation(request, response)) return;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [](const nlohmann::json& json) { parse_empty_object(json); })) return;

        log_info(session_event(key, "delete_requested"));
        MaintenanceReservationResult reserved =
            live_sessions->reserve_for_deletion(key, settings.delete_deadline);
        if (const auto* failure = std::get_if<MaintenanceFailure>(&reserved)) {
            log_warn(session_event(key, "delete_failed"));
            if (*failure == MaintenanceFailure::manager_stopping) {
                return set_error_response(response, 503,
                    {ErrorCode::server_stopping, "Server is stopping."});
            }
            return set_error_response(response, 409,
                {ErrorCode::session_stopping, "Session is stopping."});
        }
        try {
            sessions->move_to_deleted(key);
            log_info(session_event(key, "delete_moved"));
            response.status = 204;
            response.set_header("Cache-Control", "no-store");
        } catch (const ForumNotFoundError&) {
            log_warn(session_event(key, "delete_failed"));
            set_route_not_found(response);
        } catch (const SessionNotFoundError&) {
            log_warn(session_event(key, "delete_failed"));
            set_route_not_found(response);
        } catch (const SessionBusyError&) {
            log_warn(session_event(key, "delete_failed"));
            set_error_response(response, 409,
                {ErrorCode::session_busy, "Session is busy."});
        } catch (const SessionDeleteConflictError&) {
            log_warn(session_event(key, "delete_failed"));
            set_error_response(response, 409,
                {ErrorCode::session_delete_conflict,
                 "A deleted copy of this session already exists."});
        } catch (...) {
            log_warn(session_event(key, "delete_failed"));
            throw;
        }
    });

    server.Post(R"(/api/v1/forums/([^/]+)/sessions/([^/]+)/open)", [sessions, live_sessions, settings](const httplib::Request& request, httplib::Response& response) {
        const FullSessionId key{request.matches[1], request.matches[2]};
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
