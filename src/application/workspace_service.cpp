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

class PreparedRoom::Impl {
public:
    Impl(const Workspace& workspace, std::string room_name)
      : room(workspace.load_room(room_name)),
        definitions(load_definitions(workspace, room)),
        repository(room.directory / "sessions", room.name) {}

    Room room;
    std::vector<AgentDefinition> definitions;
    SessionRepository repository;
};

WorkspaceService::WorkspaceService(std::filesystem::path root)
    : impl_(std::make_unique<Impl>(std::move(root))) {}

WorkspaceService::~WorkspaceService() = default;

std::vector<std::string> WorkspaceService::rooms() const {
    return impl_->workspace.rooms();
}

PreparedRoom WorkspaceService::prepare_room(
    const std::string& room_name) const {
    return PreparedRoom(std::make_unique<PreparedRoom::Impl>(
        impl_->workspace,
        room_name));
}

PreparedRoom::PreparedRoom(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

PreparedRoom::~PreparedRoom() = default;
PreparedRoom::PreparedRoom(PreparedRoom&&) noexcept = default;
PreparedRoom& PreparedRoom::operator=(PreparedRoom&&) noexcept = default;

std::vector<SessionSummary> PreparedRoom::sessions() const {
    const std::vector<Session> stored = impl_->repository.list();
    std::vector<SessionSummary> result;
    result.reserve(stored.size());
    for (const Session& session : stored) {
        result.push_back(summarize(session));
    }
    return result;
}

std::unique_ptr<ChatCoordinator> PreparedRoom::create_session(
    std::string label) const {
    const Session session = impl_->repository.create(std::move(label));
    return std::make_unique<ChatCoordinator>(
        impl_->definitions,
        impl_->repository.database_path(session.id));
}

std::unique_ptr<ChatCoordinator> PreparedRoom::open_session(
    const std::string& session_id) const {
    const std::filesystem::path database_path =
        impl_->repository.open_database_path(session_id);
    ConversationRestore restored = load_conversation_state(database_path);
    return std::make_unique<ChatCoordinator>(
        impl_->definitions,
        database_path,
        std::move(restored));
}

} // namespace cha
