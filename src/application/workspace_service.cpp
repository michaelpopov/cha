#include "application/workspace_service.h"

#include "application/chat_coordinator.h"
#include "storage/agent_definition_loader.h"
#include "storage/session_database.h"
#include "storage/session_repository.h"
#include "storage/workspace.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cha {
namespace {

std::vector<AgentDefinition> load_definitions(
    const Workspace& workspace,
    const Room& room) {

    std::vector<std::filesystem::path> persona_directories;
    persona_directories.reserve(room.persona_names.size());
    for (const std::string& persona : room.persona_names) {
        persona_directories.push_back(
            workspace.persona_directory(persona));
    }
    return load_agent_definitions(persona_directories, room.directory);
}

SessionRepository session_repository(
    const Workspace& workspace,
    const std::string& room_name) {

    const Room room = workspace.load_room(room_name);
    return SessionRepository(room.directory / "sessions", room.name);
}

SessionSummary summarize(const Session& stored) {
    return {
        .id = stored.id,
        .label = stored.label,
        .error = stored.error,
    };
}

} // namespace

class WorkspaceService::Impl {
public:
    explicit Impl(std::filesystem::path root) : workspace(std::move(root)) {}

    Workspace workspace;
};

WorkspaceService::WorkspaceService(std::filesystem::path root)
    : impl_(std::make_unique<Impl>(std::move(root))) {}

WorkspaceService::~WorkspaceService() = default;

std::vector<std::string> WorkspaceService::rooms() const {
    return impl_->workspace.rooms();
}

std::vector<SessionSummary> WorkspaceService::sessions(
    const std::string& room_name) const {

    const SessionRepository repository =
        session_repository(impl_->workspace, room_name);

    const std::vector<Session> stored = repository.list();

    std::vector<SessionSummary> result;
    result.reserve(stored.size());
    for (const Session& session : stored) {
        result.push_back(summarize(session));
    }
    return result;
}

std::unique_ptr<ChatCoordinator> WorkspaceService::create_session(
    const std::string& room_name,
    std::string label) const {

    const Room room = impl_->workspace.load_room(room_name);
    std::vector<AgentDefinition> definitions =
        load_definitions(impl_->workspace, room);
    const SessionRepository repository(
        room.directory / "sessions",
        room.name);

    const Session session = repository.create(std::move(label));
    return ChatCoordinator::from_definitions(
        std::move(definitions),
        repository.database_path(session.id));
}

std::unique_ptr<ChatCoordinator> WorkspaceService::open_session(
    const std::string& room_name,
    const std::string& session_id) const {

    const Room room = impl_->workspace.load_room(room_name);
    std::vector<AgentDefinition> definitions =
        load_definitions(impl_->workspace, room);
    const SessionRepository repository(
        room.directory / "sessions",
        room.name);

    const std::filesystem::path database_path =
        repository.open_database_path(session_id);
    ConversationRestore restored = load_conversation_state(database_path);
    return ChatCoordinator::from_definitions(
        std::move(definitions),
        database_path,
        std::move(restored));
}

} // namespace cha
