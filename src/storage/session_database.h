#pragma once

#include "chat/session_identity.h"
#include "chat/session_character_settings.h"
#include "chat/transcript.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

struct NoteText;

using SessionKey = std::int64_t;

struct SessionDatabaseMetadata {
    std::string id;
    std::string forum;
    std::string label;
};

struct InterruptedTurn {
    RequestId request_id{};
    TranscriptEntry error_entry;
};

struct SessionRestore {
    std::vector<TranscriptEntry> entries;
    RequestId next_request_id{1};
    EntryId next_entry_id{1};
    std::vector<InterruptedTurn> interrupted_turns;
    SessionCharacterSettingsMap character_settings;
    bool recent_pending{};
    // New automatic sessions become permanent before their first submission.
    bool discardable{};
};

struct LoadedSessionDatabase {
    SessionKey session_key{};
    SessionDatabaseMetadata metadata;
    SessionRestore restore;
};

// Creates a workspace-schema database containing exactly one forum and one
// session. Production uses this for Welcome; tests use it as a compact real
// journal fixture. The inserted session_key is always 1.
[[nodiscard]] bool create_session_database(
    const std::filesystem::path& path,
    const SessionDatabaseMetadata& metadata);

SessionDatabaseMetadata read_session_database_metadata(
    const std::filesystem::path& path);

SessionRestore load_session_state(const std::filesystem::path& path);


LoadedSessionDatabase load_session_database(
    const std::filesystem::path& path,
    const FullSessionId& expected_identity,
    std::string_view password = {});

// Active-epoch durable rows only. Epoch remains part of the v2 on-disk format
// so databases cleared by older builds continue to expose the right history.
// New sessions never advance it.
std::vector<TranscriptEntry> load_session_history(
    const std::filesystem::path& path,
    const FullSessionId& expected_identity,
    std::string_view password = {});

// One live actor owns one journal connection. Different journals may point at
// the same workspace database, but every statement is scoped by session_key.
class SessionJournal {
public:
    SessionJournal(
        std::filesystem::path path,
        SessionKey session_key,
        std::string_view password = {});
    // Convenience for a database created by create_session_database().
    explicit SessionJournal(std::filesystem::path path);
    ~SessionJournal();

    SessionJournal(const SessionJournal&) = delete;
    SessionJournal& operator=(const SessionJournal&) = delete;

    void start_turn(RequestId request_id, const TranscriptEntry& prompt);
    void record_entry(const TranscriptEntry& entry);
    // Stores the note copy and its notice in one transaction.
    [[nodiscard]] TranscriptEntry add_note(const NoteText& note, EntryId entry_id);
    // Adds scope without loading the saved text; false means no copy exists.
    [[nodiscard]] bool add_note_scope(std::int64_t note_id, std::string_view character_id);
    // Saves the first copy and scope together, preserving any existing copy.
    void save_note_reference(const NoteText& note, std::string_view character_id);
    [[nodiscard]] TranscriptEntry add_text_file(
        std::string_view name, std::string_view text, EntryId entry_id);
    // Persistent attach rechecks readiness on this journal connection when
    // welcome_ready is empty. Welcome supplies a vault callback instead.
    [[nodiscard]] TranscriptEntry add_folder(
        std::int64_t folder_id,
        std::string_view folder_name,
        EntryId entry_id,
        std::function<bool(std::int64_t)> welcome_ready = {});
    [[nodiscard]] TranscriptEntry detach_folder(
        std::int64_t folder_id,
        std::string_view folder_name,
        EntryId entry_id);
    [[nodiscard]] std::vector<std::int64_t> folder_ids() const;
    void prune_folders(std::span<const std::int64_t> folder_ids);
    void clear_vault_context();
    // No target: explicit additions only (including text files).
    [[nodiscard]] std::string notes_prompt(
        std::optional<std::string_view> character_id = std::nullopt) const;
    void complete_turn(RequestId request_id, const TranscriptEntry& response);
    void cancel_turn(RequestId request_id, std::optional<TranscriptEntry> response);
    void fail_turn(RequestId request_id, const TranscriptEntry& error);
    void delete_turn(EntryId response_entry_id);
    void clear_history();
    void set_character_settings(
        std::string_view character_id, const SessionCharacterSettings& settings);
    void rename(std::string_view label);
    void publish_recent();
    void retain();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cha
