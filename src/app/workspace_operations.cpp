#include "app/workspace_operations.h"

#include "app/application.h"
#include "util/logging.h"
#include "util/text.h"
#include "web/live_session.h"
#include "web/live_session_manager.h"
#include "web/protocol.h"
#include "web/session_projection.h"
#include "workspace/builtins.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"
#include "session/session_repository.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

namespace cha::app::workspace {
namespace {

using cha::web::ErrorCode;

[[noreturn]] void fail(ErrorCode code, std::string message) {
    throw ApplicationError(code, std::move(message));
}

void apply_edit(
    cha::web::LiveSessionManager& live_sessions,
    const WorkspaceConfigEditResult& edited) {
    invalidate_affected_sessions(live_sessions, edited.affected_forum_ids);
}

template<typename Fn>
auto with_workspace_edit(Fn&& fn) {
    try {
        return fn();
    } catch (const ApplicationError&) {
        throw;
    } catch (const std::out_of_range&) {
        fail(ErrorCode::not_found, "That file was not found.");
    } catch (const WorkspaceConfigValidationError& error) {
        log_warn(std::string("Rejected workspace edit: ") + error.what());
        fail(ErrorCode::invalid_argument, error.what());
    } catch (const WorkspaceRestartRequiredError& error) {
        fail(ErrorCode::application_unavailable, error.what());
    } catch (const std::invalid_argument& error) {
        fail(ErrorCode::invalid_argument, error.what());
    }
}

cha::web::MarkdownFile edit_markdown_file(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    bool character,
    std::string_view id,
    std::string filename,
    std::optional<std::string> content,
    bool create) {
    const auto workspace = store.snapshot();
    if (character) {
        if (workspace->find_character(id) == nullptr
            || !workspace->character_is_writable(id)) {
            fail(ErrorCode::not_found, "That character file was not found.");
        }
    } else if (workspace->find_forum(id) == nullptr
        || !workspace->forum_is_writable(id)) {
        fail(ErrorCode::not_found, "That forum file was not found.");
    }
    return with_workspace_edit([&] {
        try {
            const auto edited = character
                ? store.apply_character_file(
                    id,
                    filename,
                    content ? std::optional<std::string_view>(*content)
                            : std::nullopt,
                    create)
                : store.apply_forum_file(
                    id,
                    filename,
                    content ? std::optional<std::string_view>(*content)
                            : std::nullopt,
                    create);
            apply_edit(live_sessions, edited);
        } catch (const std::out_of_range&) {
            fail(
                ErrorCode::not_found,
                character ? "That character file was not found."
                          : "That forum file was not found.");
        } catch (const std::invalid_argument&) {
            const bool required = !content
                && ((character && filename == "CHARACTER.md")
                    || (!character && filename == "FORUM.md"));
            if (required) {
                fail(
                    ErrorCode::invalid_argument,
                    character ? "CHARACTER.md is required."
                              : "FORUM.md is required.");
            }
            fail(
                ErrorCode::invalid_argument,
                content ? "Invalid file or duplicate filename."
                        : (character ? "Invalid character file."
                                     : "Invalid forum file."));
        }
        if (!content) return cha::web::MarkdownFile{};
        return markdown_file(std::move(filename), std::move(*content), true);
    });
}

} // namespace

bool is_welcome_session(
    std::string_view forum_id,
    std::string_view session_id) noexcept {
    return forum_id == entrance_id && session_id == welcome_id;
}

cha::web::CharacterSummary character_summary(
    const Workspace& workspace,
    const WorkspaceCharacter& character) {
    return {
        .id = character.character.id,
        .display_name = character.character.display_name,
        .description = character.character.description,
        .appearance = character.character.appearance,
        .voice = cha::web::resolve_speech_voice(workspace, character),
    };
}

cha::web::CharacterDetail character_detail(
    const Workspace& workspace,
    const WorkspaceCharacter& character) {
    cha::web::CharacterDetail detail{
        .summary = character_summary(workspace, character),
        .character_markdown = character.markdown,
        .editable_markdown = character.editable_markdown,
    };
    detail.writable = workspace.character_is_writable(character.character.id);
    for (const auto& [filename, content] : character.markdown_files) {
        detail.markdown_files.push_back(filename);
    }
    if (!detail.writable && detail.markdown_files.empty()) {
        detail.markdown_files.push_back("CHARACTER.md");
    }
    detail.settings_writable =
        workspace.character_settings_are_writable(character.character.id);
    if (detail.settings_writable) {
        detail.provider = character.provider_id;
        detail.style = character.style_id;
        detail.voice = character.voice_id;
        detail.reasoning_effort = character.reasoning_effort;
        detail.web_search = character.web_search;
    }
    for (const WorkspaceProvider& provider : workspace.providers()) {
        detail.available_providers.push_back({provider.id, provider.label});
    }
    for (const WorkspaceStyle& style : workspace.styles()) {
        detail.available_styles.push_back(
            {style.id, style.label, style.appearance});
    }
    for (const WorkspaceVoice& voice : workspace.voices()) {
        detail.available_voices.push_back({voice.id, voice.label});
    }
    return detail;
}

cha::web::PersonaSummary persona_summary(
    const Workspace& workspace,
    const WorkspacePersona& persona) {
    return {
        .id = persona.id,
        .display_name = persona.display_name,
        .description = persona.description,
        .appearance = persona.appearance,
        .voice = cha::web::resolve_speech_voice(workspace, persona),
    };
}

cha::web::PersonaDetail persona_detail(
    const Workspace& workspace,
    const WorkspacePersona& persona) {
    cha::web::PersonaDetail detail{
        .summary = persona_summary(workspace, persona),
        .persona_markdown = persona.prompt,
        .style = persona.style_id,
        .voice = persona.voice_id,
        .writable = workspace.persona_is_writable(persona.id),
    };
    for (const WorkspaceStyle& style : workspace.styles()) {
        detail.available_styles.push_back(
            {style.id, style.label, style.appearance});
    }
    for (const WorkspaceVoice& voice : workspace.voices()) {
        detail.available_voices.push_back({voice.id, voice.label});
    }
    return detail;
}

cha::web::ForumSummary forum_summary(
    const WorkspaceForum& forum,
    const Workspace& workspace) {
    const WorkspacePersona* persona =
        workspace.find_persona(forum.default_persona_id);
    if (persona == nullptr) {
        throw std::runtime_error(
            "Forum default persona is absent from the workspace");
    }
    cha::web::ForumSummary result{
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
            throw std::runtime_error(
                "Forum member is absent from the workspace");
        }
        result.members.push_back(character_summary(workspace, *character));
    }
    std::ranges::sort(
        result.members, {},
        [](const cha::web::CharacterSummary& character) {
            return fold_ascii(character.display_name);
        });
    return result;
}

cha::web::ForumDetail forum_detail(
    const Workspace& workspace,
    const WorkspaceForum& forum) {
    cha::web::ForumDetail detail{
        .summary = forum_summary(forum, workspace),
        .forum_markdown = forum.prompt_template,
        .writable = workspace.forum_is_writable(forum.id),
    };
    for (const auto& [filename, content] : forum.markdown_files) {
        detail.markdown_files.push_back(filename);
    }
    if (!detail.writable && detail.markdown_files.empty()) {
        detail.markdown_files.push_back("FORUM.md");
    }
    return detail;
}

cha::web::MarkdownFile markdown_file(
    std::string filename,
    std::string content,
    bool writable) {
    return {std::move(filename), std::move(content), writable};
}

std::vector<cha::web::SessionListing> sessions_for(
    const SessionRepository& sessions,
    const cha::web::LiveSessionManagerSnapshot& snapshot,
    std::string_view forum_id) {
    std::vector<cha::web::SessionListing> result;
    for (const StoredSession& stored : sessions.list(forum_id)) {
        const bool live = std::find(
            snapshot.running_sessions.begin(),
            snapshot.running_sessions.end(),
            stored.identity) != snapshot.running_sessions.end();
        result.push_back({
            stored.identity.session_id,
            stored.label,
            live,
            stored.updated_at});
    }
    return result;
}

void invalidate_affected_sessions(
    cha::web::LiveSessionManager& live_sessions,
    std::span<const std::string> forum_ids) {
    for (const auto& live : live_sessions.active_sessions()) {
        const FullSessionId& key = live->identity();
        if (std::ranges::find(forum_ids, key.forum_id) == forum_ids.end()) {
            continue;
        }
        live->request_shutdown(cha::web::ShutdownReason::reloading);
    }
}

cha::web::CharacterDetail get_character(
    const Workspace& workspace,
    std::string_view id) {
    const WorkspaceCharacter* character = workspace.find_character(id);
    if (character == nullptr) {
        fail(ErrorCode::not_found, "That character was not found.");
    }
    return character_detail(workspace, *character);
}

cha::web::CharacterDetail create_character(
    WorkspaceConfigStore& store,
    std::string_view display_name,
    std::string_view description) {
    return with_workspace_edit([&] {
        std::string id;
        try {
            id = store.create_character(display_name, description);
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid character.");
        }
        const auto current = store.snapshot();
        const WorkspaceCharacter* created = current->find_character(id);
        if (created == nullptr) {
            fail(ErrorCode::internal_error, "The character could not be created.");
        }
        return character_detail(*current, *created);
    });
}

cha::web::CharacterDetail update_character_settings(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    const cha::web::CharacterSettingsUpdate& update) {
    const auto workspace = store.snapshot();
    const WorkspaceCharacter* character = workspace->find_character(id);
    if (character == nullptr
        || !workspace->character_settings_are_writable(id)) {
        fail(ErrorCode::not_found, "That character was not found.");
    }
    const bool changed = !character->provider_id
        || update.provider != *character->provider_id
        || update.style != character->style_id
        || update.voice != character->voice_id
        || update.reasoning_effort != character->reasoning_effort
        || update.web_search != character->web_search;
    return with_workspace_edit([&] {
        try {
            if (changed) {
                const std::optional<std::string_view> style = update.style
                    ? std::optional<std::string_view>(*update.style)
                    : std::nullopt;
                const std::optional<std::string_view> reasoning_effort =
                    update.reasoning_effort
                    ? std::optional<std::string_view>(*update.reasoning_effort)
                    : std::nullopt;
                const std::optional<std::string_view> voice = update.voice
                    ? std::optional<std::string_view>(*update.voice)
                    : std::nullopt;
                apply_edit(
                    live_sessions,
                    store.apply_character_settings(
                        id, update.provider, style, voice,
                        reasoning_effort, update.web_search));
            }
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid character settings.");
        }
        const auto current = store.snapshot();
        const WorkspaceCharacter* updated = current->find_character(id);
        if (updated == nullptr) {
            fail(ErrorCode::not_found, "That character was not found.");
        }
        return character_detail(*current, *updated);
    });
}

cha::web::CharacterDetail update_character_definition(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    const cha::web::CharacterDefinitionUpdate& update) {
    const auto workspace = store.snapshot();
    const WorkspaceCharacter* character = workspace->find_character(id);
    if (character == nullptr || !workspace->character_is_writable(id)) {
        fail(ErrorCode::not_found, "That character was not found.");
    }
    const std::string& display_name = update.display_name
        ? *update.display_name : character->character.display_name;
    const bool changed = display_name != character->character.display_name
        || update.character_markdown.has_value();
    return with_workspace_edit([&] {
        try {
            if (changed) {
                apply_edit(
                    live_sessions,
                    store.apply_character_definition(
                        id,
                        display_name,
                        update.character_markdown
                            ? std::optional<std::string_view>(
                                *update.character_markdown)
                            : std::nullopt));
            }
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid character.");
        }
        const auto current = store.snapshot();
        const WorkspaceCharacter* updated = current->find_character(id);
        if (updated == nullptr) {
            fail(ErrorCode::not_found, "That character was not found.");
        }
        return character_detail(*current, *updated);
    });
}

void delete_character(WorkspaceConfigStore& store, std::string_view id) {
    const auto workspace = store.snapshot();
    if (workspace->find_character(id) == nullptr
        || !workspace->character_is_writable(id)) {
        fail(ErrorCode::not_found, "That character was not found.");
    }
    with_workspace_edit([&] {
        try {
            store.apply_character_delete(id);
        } catch (const std::invalid_argument&) {
            fail(
                ErrorCode::invalid_argument,
                "This character is still used by one or more forums.");
        }
        return 0;
    });
}

cha::web::MarkdownFile get_character_file(
    const Workspace& workspace,
    std::string_view id,
    std::string_view filename) {
    const WorkspaceCharacter* character = workspace.find_character(id);
    if (character == nullptr) {
        fail(ErrorCode::not_found, "That character file was not found.");
    }
    const auto file = character->markdown_files.find(std::string(filename));
    const bool writable = workspace.character_is_writable(id);
    if (file == character->markdown_files.end()) {
        if (!writable && character->markdown_files.empty()
            && filename == "CHARACTER.md") {
            return markdown_file(
                std::string(filename), character->editable_markdown, false);
        }
        fail(ErrorCode::not_found, "That character file was not found.");
    }
    return markdown_file(std::string(filename), file->second, writable);
}

cha::web::MarkdownFile create_character_file(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename,
    std::string content) {
    return edit_markdown_file(
        store, live_sessions, true, id, std::move(filename),
        std::move(content), true);
}

cha::web::MarkdownFile update_character_file(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename,
    std::string content) {
    return edit_markdown_file(
        store, live_sessions, true, id, std::move(filename),
        std::move(content), false);
}

void delete_character_file(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename) {
    (void)edit_markdown_file(
        store, live_sessions, true, id, std::move(filename),
        std::nullopt, false);
}

cha::web::PersonaDetail get_persona(
    const Workspace& workspace,
    std::string_view id) {
    const WorkspacePersona* persona = workspace.find_persona(id);
    if (persona == nullptr) {
        fail(ErrorCode::not_found, "That persona was not found.");
    }
    return persona_detail(workspace, *persona);
}

cha::web::PersonaDetail create_persona(
    WorkspaceConfigStore& store,
    std::string_view display_name) {
    return with_workspace_edit([&] {
        std::string id;
        try {
            id = store.create_persona(display_name);
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid persona.");
        }
        const auto current = store.snapshot();
        const WorkspacePersona* created = current->find_persona(id);
        if (created == nullptr) {
            fail(ErrorCode::internal_error, "The persona could not be created.");
        }
        return persona_detail(*current, *created);
    });
}

cha::web::PersonaDetail update_persona(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    const cha::web::PersonaUpdate& update) {
    const auto workspace = store.snapshot();
    const WorkspacePersona* persona = workspace->find_persona(id);
    if (persona == nullptr || !workspace->persona_is_writable(id)) {
        fail(ErrorCode::not_found, "That persona was not found.");
    }
    const std::string& display_name = update.display_name
        ? *update.display_name : persona->display_name;
    const std::string& markdown = update.persona_markdown
        ? *update.persona_markdown : persona->prompt;
    const std::optional<std::string> style = update.style
        ? *update.style : persona->style_id;
    const std::optional<std::string> voice = update.voice
        ? *update.voice : persona->voice_id;
    const bool changed = display_name != persona->display_name
        || markdown != persona->prompt
        || style != persona->style_id
        || voice != persona->voice_id;
    return with_workspace_edit([&] {
        try {
            if (changed) {
                apply_edit(
                    live_sessions,
                    store.apply_persona_update(
                        id,
                        display_name,
                        markdown,
                        style ? std::optional<std::string_view>(*style)
                              : std::nullopt,
                        voice ? std::optional<std::string_view>(*voice)
                              : std::nullopt));
            }
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid persona.");
        }
        const auto current = store.snapshot();
        const WorkspacePersona* updated = current->find_persona(id);
        if (updated == nullptr) {
            fail(ErrorCode::not_found, "That persona was not found.");
        }
        return persona_detail(*current, *updated);
    });
}

void delete_persona(WorkspaceConfigStore& store, std::string_view id) {
    const auto workspace = store.snapshot();
    if (workspace->find_persona(id) == nullptr
        || !workspace->persona_is_writable(id)) {
        fail(ErrorCode::not_found, "That persona was not found.");
    }
    with_workspace_edit([&] {
        try {
            store.apply_persona_delete(id);
        } catch (const std::invalid_argument&) {
            fail(
                ErrorCode::invalid_argument,
                "This persona is still used by one or more forums.");
        }
        return 0;
    });
}

cha::web::ForumDetail get_forum(
    const Workspace& workspace,
    std::string_view id) {
    const WorkspaceForum* forum = workspace.find_forum(id);
    if (forum == nullptr) {
        fail(ErrorCode::not_found, "That forum was not found.");
    }
    return forum_detail(workspace, *forum);
}

cha::web::ForumDetail create_forum(
    WorkspaceConfigStore& store,
    std::string_view display_name,
    std::string_view persona_id) {
    return with_workspace_edit([&] {
        std::string id;
        try {
            id = store.create_forum(display_name, persona_id);
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid forum.");
        }
        const auto current = store.snapshot();
        const WorkspaceForum* created = current->find_forum(id);
        if (created == nullptr) {
            fail(ErrorCode::internal_error, "The forum could not be created.");
        }
        return forum_detail(*current, *created);
    });
}

cha::web::ForumDetail update_forum(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    const cha::web::ForumUpdate& update) {
    const auto workspace = store.snapshot();
    const WorkspaceForum* forum = workspace->find_forum(id);
    if (forum == nullptr || !workspace->forum_is_writable(id)) {
        fail(ErrorCode::not_found, "That forum was not found.");
    }
    const std::string& display_name = update.display_name
        ? *update.display_name : forum->display_name;
    const std::string& markdown = update.forum_markdown
        ? *update.forum_markdown : forum->prompt_template;
    const bool changed = display_name != forum->display_name
        || markdown != forum->prompt_template;
    return with_workspace_edit([&] {
        try {
            if (changed) {
                apply_edit(
                    live_sessions,
                    store.apply_forum_update(id, display_name, markdown));
            }
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid forum.");
        }
        const auto current = store.snapshot();
        const WorkspaceForum* updated = current->find_forum(id);
        if (updated == nullptr) {
            fail(ErrorCode::not_found, "That forum was not found.");
        }
        return forum_detail(*current, *updated);
    });
}

void delete_forum(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id) {
    const auto workspace = store.snapshot();
    if (workspace->find_forum(id) == nullptr
        || !workspace->forum_is_writable(id)) {
        fail(ErrorCode::not_found, "That forum was not found.");
    }
    with_workspace_edit([&] {
        apply_edit(live_sessions, store.apply_forum_delete(id));
        return 0;
    });
}

cha::web::ForumDetail update_forum_members(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    const cha::web::ForumMembersUpdate& update) {
    const auto workspace = store.snapshot();
    const WorkspaceForum* forum = workspace->find_forum(id);
    if (forum == nullptr || !workspace->forum_is_writable(id)) {
        fail(ErrorCode::not_found, "That forum was not found.");
    }
    return with_workspace_edit([&] {
        try {
            apply_edit(
                live_sessions,
                store.apply_forum_members_and_persona(
                    id, update.character_ids, update.persona_id));
        } catch (const std::invalid_argument&) {
            fail(
                ErrorCode::invalid_argument,
                "Select a configured persona and at least one configured "
                "character.");
        }
        const auto current = store.snapshot();
        const WorkspaceForum* updated = current->find_forum(id);
        if (updated == nullptr) {
            fail(ErrorCode::not_found, "That forum was not found.");
        }
        return forum_detail(*current, *updated);
    });
}

cha::web::MarkdownFile get_forum_file(
    const Workspace& workspace,
    std::string_view id,
    std::string_view filename) {
    const WorkspaceForum* forum = workspace.find_forum(id);
    if (forum == nullptr) {
        fail(ErrorCode::not_found, "That forum file was not found.");
    }
    const auto file = forum->markdown_files.find(std::string(filename));
    const bool writable = workspace.forum_is_writable(id);
    if (file == forum->markdown_files.end()) {
        if (!writable && forum->markdown_files.empty()
            && filename == "FORUM.md") {
            return markdown_file(
                std::string(filename), forum->prompt_template, false);
        }
        fail(ErrorCode::not_found, "That forum file was not found.");
    }
    return markdown_file(std::string(filename), file->second, writable);
}

cha::web::MarkdownFile create_forum_file(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename,
    std::string content) {
    return edit_markdown_file(
        store, live_sessions, false, id, std::move(filename),
        std::move(content), true);
}

cha::web::MarkdownFile update_forum_file(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename,
    std::string content) {
    return edit_markdown_file(
        store, live_sessions, false, id, std::move(filename),
        std::move(content), false);
}

void delete_forum_file(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename) {
    (void)edit_markdown_file(
        store, live_sessions, false, id, std::move(filename),
        std::nullopt, false);
}

} // namespace cha::app::workspace
