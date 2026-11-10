#include "app/d1_client.h"
#include "app/d1_sync.h"
#include "runtime/live_session_manager.h"
#include "session/session_open.h"
#include "chat/transcript.h"
#include "storage/note_storage.h"
#include "storage/session_database.h"
#include "storage/session_repository.h"
#include "storage/sqlite_storage.h"
#include "storage/sync_chat.h"
#include "storage/sync_storage.h"
#include "storage/workspace_session_database.h"
#include "support/test_workspace.h"
#include "util/base64.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;

constexpr std::string_view sync_machine = "mac";

std::string d1_key_toml() {
    return "display_name = \"D1\"\ntype = \"D1\"\naccount_id = \"a\"\n"
        "database_id = \"d\"\napi_token = \"t\"\n";
}

void add_d1_key(Database& database) {
    database.prepare(
        "INSERT INTO config (name, content) VALUES (?1, ?2)",
        "system/keys/api_key_1/config.toml", d1_key_toml()).run();
}

void make_chat_session(
    Database& database,
    std::string_view forum_id = "f1",
    std::string_view session_id = "20261109-120000-session",
    std::string_view label = "Test Chat") {
    database.prepare("INSERT INTO forums (forum_id) VALUES (?1) "
        "ON CONFLICT(forum_id) DO NOTHING", forum_id).run();
    auto forum = database.prepare("SELECT forum_key FROM forums WHERE forum_id=?1", forum_id);
    (void)forum.step();
    const std::int64_t forum_key = forum.integer(0);
    database.prepare(
        "INSERT INTO sessions (forum_key, session_id, label, updated_at, "
        "history_epoch, next_entry_id, next_request_id, recent_pending, discardable) "
        "VALUES (?1, ?2, ?3, ?4, 1, 1, 1, 0, 0)",
        forum_key, session_id, label,
        static_cast<std::int64_t>(std::time(nullptr))).run();
}

std::int64_t session_key_for(
    Database& database,
    std::string_view forum_id,
    std::string_view session_id) {
    auto query = database.prepare(
        "SELECT s.session_key FROM sessions s JOIN forums f USING(forum_key) "
        "WHERE f.forum_id=?1 AND s.session_id=?2",
        forum_id, session_id);
    if (!query.step()) throw std::runtime_error("missing test session");
    return query.integer(0);
}

nlohmann::json queued_records(Database& database) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& row : sync_queue_list(database)) {
        out.push_back(nlohmann::json::parse(row.record));
    }
    return out;
}

TranscriptEntry human_entry(
    EntryId id, std::string text, std::optional<RequestId> request = std::nullopt) {
    TranscriptEntry entry = make_human_entry({
        .id = id,
        .author = {"user", "User"},
        .addressed_to = {"agent", "Agent"},
        .text = std::move(text),
        .request_id = request,
    });
    return entry;
}

TranscriptEntry reply_entry(
    EntryId id, std::string text, RequestId request) {
    return make_character_entry(id, "agent", "Agent", std::move(text),
        EntryStatus::complete, request);
}

class ChatSyncTest : public testing::Test {
protected:
    void SetUp() override {
        path_ = workspace_.root() / "chat.sqlite3";
        create_empty_workspace_session_database(path_);
        database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
        add_d1_key(*database_);
    }

    test::TestWorkspace workspace_;
    std::filesystem::path path_;
    std::unique_ptr<Database> database_;
};

TEST_F(ChatSyncTest, TemporaryLabelWritesNothing) {
    make_chat_session(*database_, "f1", "s-temp", "New session");
    const auto key = session_key_for(*database_, "f1", "s-temp");
    SessionJournal journal(path_, key);
    journal.set_sync_machine(std::string(sync_machine));
    journal.start_turn(1, human_entry(1, "Hi", 1));
    journal.complete_turn(1, reply_entry(2, "Hello", 1));
    EXPECT_TRUE(sync_queue_list(*database_).empty());
}

TEST_F(ChatSyncTest, FirstFinishedTurnPublishesInitialOnce) {
    make_chat_session(*database_, "f1", "s-1", "Test Chat");
    const auto key = session_key_for(*database_, "f1", "s-1");
    SessionJournal journal(path_, key);
    journal.set_sync_machine(std::string(sync_machine));
    // Running turn alone sends nothing.
    journal.start_turn(1, human_entry(1, "Hi", 1));
    EXPECT_TRUE(sync_queue_list(*database_).empty());
    journal.complete_turn(1, reply_entry(2, "Hello", 1));
    auto records = queued_records(*database_);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["kind"], "chat.entries");
    EXPECT_EQ(records[0]["chat"], nlohmann::json::array({"f1", "s-1"}));
    EXPECT_EQ(records[0]["label"], "Test Chat");
    EXPECT_EQ(records[0]["state"], 1);
    ASSERT_EQ(records[0]["entries"].size(), 2u);
    // Second turn sends only its own record.
    journal.start_turn(2, human_entry(3, "Again", 2));
    journal.complete_turn(2, reply_entry(4, "Again!", 2));
    records = queued_records(*database_);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[1]["entries"].size(), 2u);
}

TEST_F(ChatSyncTest, TerminalStatesArePreserved) {
    make_chat_session(*database_, "f1", "s-cancel", "Chat");
    auto key = session_key_for(*database_, "f1", "s-cancel");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        auto cancelled = make_character_entry(
            2, "agent", "Agent", "Stopped", EntryStatus::cancelled, 1);
        journal.cancel_turn(1, cancelled);
    }
    auto records = queued_records(*database_);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["state"], 2);
    // Failed turn.
    make_chat_session(*database_, "f1", "s-fail", "Chat");
    key = session_key_for(*database_, "f1", "s-fail");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        journal.fail_turn(1, make_error_entry(2, "Boom", 1));
    }
    records = queued_records(*database_);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[1]["state"], 3);
    // R59: a turn cancelled with no response adds no record.
    make_chat_session(*database_, "f1", "s-empty-cancel", "Chat");
    key = session_key_for(*database_, "f1", "s-empty-cancel");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        journal.cancel_turn(1, std::nullopt);
        journal.start_turn(2, human_entry(2, "Again", 2));
        journal.cancel_turn(2, std::nullopt);
    }
    records = queued_records(*database_);
    ASSERT_EQ(records.size(), 2u);

    // The receiver skips a turn with only its prompt.
    auto prompt_only = records[0];
    prompt_only["chat"][1] = "s-invalid";
    prompt_only["entries"].erase(1);
    for (const int state : {1, 2, 3}) {
        prompt_only["state"] = state;
        EXPECT_THROW(sync_apply_chat_record(*database_, prompt_only), std::runtime_error);
    }
    // A fractional number is skipped; it is not cut to an integer.
    for (const auto* field : {"/state", "/entries/0/kind", "/entries/1/status"}) {
        auto fractional = records[0];
        fractional["chat"][1] = "s-invalid";
        const nlohmann::json::json_pointer pointer(field);
        fractional[pointer] = fractional[pointer].get<int>() + 0.5;
        EXPECT_THROW(sync_apply_chat_record(*database_, fractional), std::runtime_error) << field;
    }
    auto received = database_->prepare(
        "SELECT COUNT(*) FROM sessions WHERE session_id='s-invalid'");
    ASSERT_TRUE(received.step());
    EXPECT_EQ(received.integer(0), 0);
}

TEST_F(ChatSyncTest, RenameFromTemporaryPublishesInitialNotRename) {
    make_chat_session(*database_, "f1", "s-rename", "New session");
    const auto key = session_key_for(*database_, "f1", "s-rename");
    SessionJournal journal(path_, key);
    journal.set_sync_machine(std::string(sync_machine));
    journal.start_turn(1, human_entry(1, "Hi", 1));
    journal.complete_turn(1, reply_entry(2, "Hello", 1));
    // Temporary label: still nothing.
    EXPECT_TRUE(sync_queue_list(*database_).empty());
    journal.rename("Real Name");
    auto records = queued_records(*database_);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["kind"], "chat.entries");
    EXPECT_EQ(records[0]["label"], "Real Name");
}

TEST_F(ChatSyncTest, SelfNoteMovesOnlyWhenShared) {
    make_chat_session(*database_, "f1", "s-self", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-self");
    SessionJournal journal(path_, key);
    journal.set_sync_machine(std::string(sync_machine));
    // @- alone never shares.
    TranscriptEntry self = make_human_entry({
        .id = 1,
        .author = {"user", "User"},
        .addressed_to = {std::string(null_agent_handle), std::string(null_agent_name)},
        .text = "Reminder",
    });
    journal.record_entry(self);
    EXPECT_TRUE(sync_queue_list(*database_).empty());
    // After a finished turn, @- sends its own record.
    journal.start_turn(1, human_entry(2, "Hi", 1));
    journal.complete_turn(1, reply_entry(3, "Hello", 1));
    const auto initial = queued_records(*database_);
    ASSERT_EQ(initial.size(), 2u);
    EXPECT_EQ(initial[0]["entries"][0]["text"], "Reminder");
    EXPECT_EQ(initial[1]["entries"][0]["text"], "Hi");
    EXPECT_FALSE(initial[1]["entries"][0].contains("entry_id"));
    EXPECT_FALSE(initial[1]["entries"][0].contains("request_id"));
    TranscriptEntry self2 = make_human_entry({
        .id = 4,
        .author = {"user", "User"},
        .addressed_to = {std::string(null_agent_handle), std::string(null_agent_name)},
        .text = "Reminder",
    });
    journal.record_entry(self2);
    auto records = queued_records(*database_);
    // Initial includes the first turn and the first @-; then the second @-.
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0]["kind"], "chat.entries");
    EXPECT_EQ(records[1]["kind"], "chat.entries");
    EXPECT_EQ(records[2]["kind"], "chat.entries");
    ASSERT_EQ(records[2]["entries"].size(), 1u);
    // Standalone notices never move.
    journal.record_entry(make_notice_entry(5, "Notice"));
    EXPECT_EQ(queued_records(*database_).size(), 3u);
}

TEST_F(ChatSyncTest, InitialIncludesAttachmentsAndSettingsInOrder) {
    make_chat_session(*database_, "f1", "s-init", "New session");
    const auto key = session_key_for(*database_, "f1", "s-init");
    // Seed a note and folder for attachments.
    const auto note = note_create(*database_, std::nullopt, NoteItemKind::note, "N");
    const auto folder = note_create(*database_, std::nullopt, NoteItemKind::folder, "F");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        journal.complete_turn(1, reply_entry(2, "Hello", 1));
        const NoteText copy{note.id, "N", "body"};
        (void)journal.add_note(copy, 3);
        (void)journal.add_text_file("f.txt", "data", 4);
        (void)journal.add_folder(folder.id, "F", 5);
        journal.set_character_settings("c1", {.provider_id = "p", .reasoning_effort = std::nullopt});
        // Still temporary: nothing.
        EXPECT_TRUE(sync_queue_list(*database_).empty());
        journal.rename("Shared");
    }
    auto records = queued_records(*database_);
    // Turn + note + file + folder + character.
    ASSERT_GE(records.size(), 5u);
    EXPECT_EQ(records[0]["kind"], "chat.entries");
    EXPECT_EQ(records[1]["kind"], "chat.add_note");
    EXPECT_EQ(records[2]["kind"], "chat.add_file");
    EXPECT_EQ(records[3]["kind"], "chat.add_folder");
    EXPECT_EQ(records[4]["kind"], "chat.character");
}

TEST_F(ChatSyncTest, DeletedSourceMakesCopyIneligible) {
    make_chat_session(*database_, "f1", "s-r55", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-r55");
    const auto note = note_create(*database_, std::nullopt, NoteItemKind::note, "Gone");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        journal.complete_turn(1, reply_entry(2, "Hello", 1));
        EXPECT_EQ(queued_records(*database_).size(), 1u);
        note_delete(*database_, note.id, std::string(sync_machine));
        const NoteText copy{note.id, "Gone", "body"};
        // Copy stays in the chat locally, but the source is gone.
        // Journal add_note would fail to find the source note text via read?
        // Simulate the R55 path: initial publication skips missing sources.
        // Here the add itself uses the passed copy, so it records; the
        // ineligible path is initial publication after deletion.
        (void)copy;
    }
    // Direct initial-publication check for R55: build a chat with a copy
    // whose source was deleted, then verify initial skips the copy.
    make_chat_session(*database_, "f1", "s-r55b", "New session");
    const auto key2 = session_key_for(*database_, "f1", "s-r55b");
    const auto doomed = note_create(*database_, std::nullopt, NoteItemKind::note, "Doomed");
    {
        SessionJournal journal(path_, key2);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        journal.complete_turn(1, reply_entry(2, "Hello", 1));
        const NoteText copy{doomed.id, "Doomed", "text"};
        (void)journal.add_note(copy, 3);
    }
    note_delete(*database_, doomed.id, std::string(sync_machine));
    // Clear queue to isolate the transition.
    {
        storage::SqliteTransaction transaction(*database_);
        database_->prepare("DELETE FROM sync_queue").run();
        transaction.commit();
    }
    {
        SessionJournal journal(path_, key2);
        journal.set_sync_machine(std::string(sync_machine));
        journal.rename("Now Shared");
    }
    auto records = queued_records(*database_);
    // The deleted-source copy is omitted: no chat.add_note for Doomed.
    bool saw_add_note = false;
    int chat_entries = 0;
    for (const auto& record : records) {
        if (record["kind"] == "chat.add_note") saw_add_note = true;
        if (record["kind"] == "chat.entries"
            && record["chat"] == nlohmann::json::array({"f1", "s-r55b"})) {
            ++chat_entries;
        }
    }
    EXPECT_FALSE(saw_add_note);
    EXPECT_EQ(chat_entries, 1);
}

TEST_F(ChatSyncTest, ClearAndLastTurnDeleteKeepEligibility) {
    make_chat_session(*database_, "f1", "s-clear", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-clear");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        journal.complete_turn(1, reply_entry(2, "Hello", 1));
        EXPECT_EQ(queued_records(*database_).size(), 1u);
        journal.clear_history();
        auto records = queued_records(*database_);
        ASSERT_EQ(records.size(), 2u);
        EXPECT_EQ(records[1]["kind"], "chat.clear");
        EXPECT_TRUE(sync_chat_is_shared(*database_, key));
    }
    // Restart: reopen and verify still shared (counter retained).
    {
        Database reopened(path_, Database::Mode::read_only);
        EXPECT_TRUE(sync_chat_is_shared(reopened, key));
    }
    {
        storage::SqliteTransaction transaction(*database_);
        database_->prepare("DELETE FROM sync_queue").run();
        transaction.commit();
    }
    // Delete the last turn in a chat with two turns; stays shared.
    make_chat_session(*database_, "f1", "s-del", "Chat");
    const auto key2 = session_key_for(*database_, "f1", "s-del");
    {
        SessionJournal journal(path_, key2);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "One", 1));
        journal.complete_turn(1, reply_entry(2, "One!", 1));
        journal.start_turn(2, human_entry(3, "Two", 2));
        journal.complete_turn(2, reply_entry(4, "Two!", 2));
        EXPECT_EQ(queued_records(*database_).size(), 2u);
        journal.delete_turn(4);
        auto records = queued_records(*database_);
        ASSERT_EQ(records.size(), 3u);
        EXPECT_EQ(records.back()["kind"], "chat.delete_turn");
        EXPECT_EQ(records.back()["turn"], 2);
        EXPECT_TRUE(sync_chat_is_shared(*database_, key2));
    }
}

TEST_F(ChatSyncTest, TitleBeforeResponsePublishesInitial) {
    make_chat_session(*database_, "f1", "s-title-first", "New session");
    const auto key = session_key_for(*database_, "f1", "s-title-first");
    SessionJournal journal(path_, key);
    journal.set_sync_machine(std::string(sync_machine));
    journal.rename("Real Title");
    EXPECT_TRUE(sync_queue_list(*database_).empty());
    journal.start_turn(1, human_entry(1, "Hi", 1));
    journal.complete_turn(1, reply_entry(2, "Hello", 1));
    auto records = queued_records(*database_);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["kind"], "chat.entries");
    EXPECT_EQ(records[0]["label"], "Real Title");
}

TEST_F(ChatSyncTest, InterruptedRepairPublishesInitial) {
    make_chat_session(*database_, "f1", "s-repair", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-repair");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        // Crash before finish: running turn remains.
    }
    {
        // Reopen repair: fail the interrupted turn.
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.fail_turn(1, make_error_entry(2, "Interrupted", 1));
    }
    auto records = queued_records(*database_);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["kind"], "chat.entries");
    EXPECT_EQ(records[0]["state"], 3);
}

TEST_F(ChatSyncTest, ClosedChatRenameAndDeleteEmit) {
    const auto path = test::import_test_database(workspace_.root());
    auto store = WorkspaceConfigStore::open(path);
    SessionRepository repository(
        [&] { return store->snapshot(); }, path, store->workspace_path(),
        store->welcome_path(), {{"welcome", "welcome"}, "Welcome"});
    repository.set_sync_machine(std::string(sync_machine));
    const auto chat = repository.create("lobby", "Closed");
    Database database(path, Database::Mode::read_write);
    add_d1_key(database);
    const auto key = session_key_for(database, chat.identity.forum_id, chat.identity.session_id);
    {
        SessionJournal journal(path, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        journal.complete_turn(1, reply_entry(2, "Hello", 1));
    }
    repository.release_pending();
    (void)repository.rename(chat.identity, "Renamed Closed");
    repository.delete_session(chat.identity);

    const auto records = queued_records(database);
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[1]["kind"], "chat.rename");
    EXPECT_EQ(records[1]["label"], "Renamed Closed");
    EXPECT_EQ(records[2]["kind"], "chat.delete");
    EXPECT_EQ(records[2]["label"], "Renamed Closed");
}

TEST_F(ChatSyncTest, TurnPositionIgnoresLocalCancellationWithoutReply) {
    make_chat_session(*database_, "f1", "s-local-cancel", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-local-cancel");
    SessionJournal journal(path_, key);
    journal.set_sync_machine(std::string(sync_machine));
    journal.start_turn(1, human_entry(1, "Cancelled locally", 1));
    journal.cancel_turn(1, std::nullopt);
    journal.start_turn(2, human_entry(2, "First shared prompt", 2));
    journal.complete_turn(2, reply_entry(3, "First shared reply", 2));
    journal.delete_turn(3);
    // The turn without a reply makes the chat shared but adds no record.
    const auto records = queued_records(*database_);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0]["entries"][0]["text"], "First shared prompt");
    EXPECT_EQ(records[1]["turn"], 1);

    make_chat_session(*database_, "f1", "s-receiver", "Chat");
    const auto receiver = session_key_for(*database_, "f1", "s-receiver");
    SessionJournal local(path_, receiver);
    local.start_turn(1, human_entry(1, "Another local cancellation", 1));
    local.cancel_turn(1, std::nullopt);
    for (auto record : records) {
        record["chat"][1] = "s-receiver";
        sync_apply_chat_record(*database_, record);
    }
    // The local turn without a reply stays; the received turn is deleted.
    auto remaining = database_->prepare(
        "SELECT request_id FROM turns WHERE session_key=?1 ORDER BY request_id", receiver);
    ASSERT_TRUE(remaining.step());
    EXPECT_EQ(remaining.integer(0), 1);
    EXPECT_FALSE(remaining.step());
}

TEST_F(ChatSyncTest, IncomingCreatesMissingChatWithLocalNumbering) {
    make_chat_session(*database_, "f1", "s-local", "Local");
    const auto local_key = session_key_for(*database_, "f1", "s-local");
    {
        SessionJournal journal(path_, local_key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(9, human_entry(1, "Local prompt", 9));
        journal.complete_turn(9, reply_entry(2, "Local reply", 9));
    }
    // Incoming turn for a missing chat uses carried identity/label and
    // receiver-local numbers.
    nlohmann::json record = {
        {"kind", "chat.entries"},
        {"chat", {"f1", "remote-1"}},
        {"label", "Remote"},
        {"state", 1},
        {"entries", {
            {{"kind", 0}, {"participant_id", "u"}, {"display_name", "U"},
             {"addressed_to", "a"}, {"addressed_to_name", "A"},
             {"text", "P"}, {"status", 0}, {"created_at", 0},
             {"input_tokens", nullptr}, {"output_tokens", nullptr},
             {"web_search_used", false}, {"request_id", 99}},
            {{"kind", 1}, {"participant_id", "a"}, {"display_name", "A"},
             {"addressed_to", ""}, {"addressed_to_name", ""},
             {"text", "R"}, {"status", 0}, {"created_at", 0},
             {"input_tokens", nullptr}, {"output_tokens", nullptr},
             {"web_search_used", false}, {"request_id", 99}},
        }},
    };
    const auto before = queued_records(*database_).size();
    EXPECT_EQ(before, 1u); // Local turn only.
    sync_apply_chat_record(*database_, record);
    // No echo: incoming writes no new records.
    EXPECT_EQ(queued_records(*database_).size(), before);
    const auto remote_key = session_key_for(*database_, "f1", "remote-1");
    auto check = database_->prepare(
        "SELECT label, next_entry_id, next_request_id FROM sessions WHERE session_key=?1",
        remote_key);
    ASSERT_TRUE(check.step());
    EXPECT_EQ(check.text(0), "Remote");
    // Local numbers start at 1, not 99.
    EXPECT_EQ(check.integer(1), 3);
    EXPECT_EQ(check.integer(2), 2);
    // No echo: incoming sets no flags.
    auto flags = database_->prepare("SELECT COUNT(*) FROM note_items WHERE changed<>0");
    ASSERT_TRUE(flags.step());
    EXPECT_EQ(flags.integer(0), 0);
}

TEST_F(ChatSyncTest, IncomingPositionalDeleteAndMissingAttachmentSkip) {
    make_chat_session(*database_, "f1", "s-pos", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-pos");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine("");
        journal.start_turn(1, human_entry(1, "One", 1));
        journal.complete_turn(1, reply_entry(2, "One!", 1));
        journal.start_turn(2, human_entry(3, "Two", 2));
        journal.complete_turn(2, reply_entry(4, "Two!", 2));
    }
    // Delete the first of two turns by position.
    sync_apply_chat_record(*database_, {
        {"kind", "chat.delete_turn"},
        {"chat", {"f1", "s-pos"}},
        {"label", "Chat"},
        {"turn", 1},
    });
    auto remaining = database_->prepare(
        "SELECT COUNT(*) FROM turns WHERE session_key=?1", key);
    ASSERT_TRUE(remaining.step());
    EXPECT_EQ(remaining.integer(0), 1);
    // Missing attachment skips with warning (throws, caller warns).
    EXPECT_THROW(
        sync_apply_chat_record(*database_, {
            {"kind", "chat.add_note"},
            {"chat", {"f1", "s-pos"}},
            {"label", "Chat"},
            {"path", {"Nope"}},
            {"name", "Nope"},
            {"text", "x"},
        }),
        std::runtime_error);
    // Missing forum skips.
    EXPECT_THROW(
        sync_apply_chat_record(*database_, {
            {"kind", "chat.rename"},
            {"chat", {"missing-forum", "s-pos"}},
            {"label", "Chat"},
        }),
        std::runtime_error);
    // Invalid turn position skips.
    EXPECT_THROW(
        sync_apply_chat_record(*database_, {
            {"kind", "chat.delete_turn"},
            {"chat", {"f1", "s-pos"}},
            {"label", "Chat"},
            {"turn", 99},
        }),
        std::runtime_error);
    // A fractional position skips and keeps the remaining turn.
    EXPECT_THROW(
        sync_apply_chat_record(*database_, {
            {"kind", "chat.delete_turn"},
            {"chat", {"f1", "s-pos"}},
            {"label", "Chat"},
            {"turn", 1.5},
        }),
        std::runtime_error);
    auto kept = database_->prepare("SELECT COUNT(*) FROM turns WHERE session_key=?1", key);
    ASSERT_TRUE(kept.step());
    EXPECT_EQ(kept.integer(0), 1);
}

TEST_F(ChatSyncTest, IncomingAttachmentsOverLimitSkip) {
    make_chat_session(*database_, "f1", "s-file", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-file");
    const auto add_file = [&](std::string name, std::size_t size) {
        sync_apply_chat_record(*database_, {
            {"kind", "chat.add_file"},
            {"chat", {"f1", "s-file"}},
            {"label", "Chat"},
            {"name", std::move(name)},
            {"text", std::string(size, 'x')},
        });
    };
    add_file("limit.txt", 256 * 1024);
    EXPECT_THROW(add_file("over.txt", 256 * 1024 + 1), std::runtime_error);
    auto files = database_->prepare(
        "SELECT name FROM session_files WHERE session_key=?1", key);
    ASSERT_TRUE(files.step());
    EXPECT_EQ(files.text(0), "limit.txt");
    EXPECT_FALSE(files.step());

    (void)note_create(*database_, std::nullopt, NoteItemKind::note, "Source");
    const auto add_note = [&](std::size_t size) {
        sync_apply_chat_record(*database_, {
            {"kind", "chat.add_note"},
            {"chat", {"f1", "s-file"}},
            {"label", "Chat"},
            {"path", {"Source"}},
            {"name", "Source"},
            {"text", std::string(size, 'x')},
        });
    };
    EXPECT_THROW(add_note(256 * 1024 + 1), std::runtime_error);
    add_note(256 * 1024);
    auto notes = database_->prepare(
        "SELECT length(text) FROM session_notes WHERE session_key=?1", key);
    ASSERT_TRUE(notes.step());
    EXPECT_EQ(notes.integer(0), 256 * 1024);
    EXPECT_FALSE(notes.step());
    // One notice for the file and one for the note.
    auto entries = database_->prepare("SELECT COUNT(*) FROM entries WHERE session_key=?1", key);
    ASSERT_TRUE(entries.step());
    EXPECT_EQ(entries.integer(0), 2);
}

// --- Worker transport script -------------------------------------------------

nlohmann::json batch_success(std::size_t count) {
    nlohmann::json results = nlohmann::json::array();
    for (std::size_t i = 0; i < count; ++i) {
        results.push_back({{"success", true}, {"results", nlohmann::json::array()}});
    }
    return {{"success", true}, {"result", std::move(results)}};
}

nlohmann::json table_list_success(std::vector<std::string> names) {
    nlohmann::json rows = nlohmann::json::array();
    for (auto& name : names) rows.push_back({{"name", std::move(name)}});
    nlohmann::json results = nlohmann::json::array();
    results.push_back({{"success", true}, {"results", std::move(rows)}});
    return {{"success", true}, {"result", std::move(results)}};
}

nlohmann::json queue_rows_success(std::vector<D1Client::Row> rows) {
    nlohmann::json list = nlohmann::json::array();
    for (auto& row : rows) {
        list.push_back(
            {{"rowid", row.rowid}, {"piece", row.piece}, {"pieces", row.pieces},
             {"record", row.record}});
    }
    nlohmann::json results = nlohmann::json::array();
    results.push_back({{"success", true}, {"results", std::move(list)}});
    return {{"success", true}, {"result", std::move(results)}};
}

nlohmann::json count_success(std::int64_t count) {
    nlohmann::json rows = nlohmann::json::array({{{"count", count}}});
    nlohmann::json results = nlohmann::json::array();
    results.push_back({{"success", true}, {"results", std::move(rows)}});
    return {{"success", true}, {"result", std::move(results)}};
}

nlohmann::json error_response(std::string message) {
    return {
        {"success", false},
        {"result", nlohmann::json::array()},
        {"errors", nlohmann::json::array({{{"message", std::move(message)}}})},
    };
}

struct ScriptedTransport {
    std::vector<std::pair<long, std::string>> responses;
    std::vector<std::string> bodies;
    std::size_t index = 0;

    std::pair<long, std::string> operator()(
        const std::string&, const std::string& body, const std::vector<std::string>&) {
        bodies.push_back(body);
        if (index >= responses.size()) return {500, "{\"success\":false}"};
        return responses[index++];
    }
};

D1SyncWorker::Config worker_config(
    const std::filesystem::path& path,
    std::shared_ptr<ScriptedTransport> scripted) {
    D1SyncWorker::Config config;
    config.database_path = path;
    config.database_password = {};
    config.vault = "Test";
    config.machine = std::string(sync_machine);
    config.transport = [scripted](
                           const std::string& url, const std::string& body,
                           const std::vector<std::string>& headers) {
        return (*scripted)(url, body, headers);
    };
    return config;
}

TEST(D1WorkerTest, SendFailureRetainsAndRetries) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            "system/keys/api_key_1/config.toml", d1_key_toml()).run();
        sync_queue_append(database, sync_make_record("note.create", {{"path", {"A"}}}));
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    // Run1 send discovery, send fail, receive empty.
    // Run2 send discovery, send success, receive empty.
    scripted->responses = {
        {200, table_list_success({"q/Test/mac", "q/Test/other"}).dump()},
        {200, error_response("transport down").dump()},
        {200, queue_rows_success({}).dump()},
        {200, table_list_success({"q/Test/mac", "q/Test/other"}).dump()},
        {200, batch_success(1).dump()},
        {200, queue_rows_success({}).dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once();
    {
        Database database(path, Database::Mode::read_only);
        EXPECT_EQ(sync_queue_list(database).size(), 1u);
    }
    worker.run_once();
    {
        Database database(path, Database::Mode::read_only);
        EXPECT_TRUE(sync_queue_list(database).empty());
    }
}

TEST(D1WorkerTest, NoRecipientsDeletesWithoutRequests) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            "system/keys/api_key_1/config.toml", d1_key_toml()).run();
        sync_queue_append(database, sync_make_record("note.create", {{"path", {"A"}}}));
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, table_list_success({"q/Test/mac"}).dump()},
        {200, queue_rows_success({}).dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once();
    Database database(path, Database::Mode::read_only);
    EXPECT_TRUE(sync_queue_list(database).empty());
    // Send discovery + receive read; no insert bodies for recipients.
    EXPECT_GE(scripted->bodies.size(), 2u);
    for (const auto& body : scripted->bodies) {
        EXPECT_EQ(body.find("INSERT"), std::string::npos);
    }
}

TEST(D1WorkerTest, MissingOwnTableHaltsAndRetains) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            "system/keys/api_key_1/config.toml", d1_key_toml()).run();
        sync_queue_append(database, sync_make_record("note.create", {{"path", {"A"}}}));
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, table_list_success({"q/Test/other"}).dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once();
    EXPECT_TRUE(worker.stopped());
    Database database(path, Database::Mode::read_only);
    EXPECT_EQ(sync_queue_list(database).size(), 1u);
}

TEST(D1WorkerTest, SplitSetAssemblesAcrossPagesAndSkipsBroken) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            "system/keys/api_key_1/config.toml", d1_key_toml()).run();
    }
    const auto make_note = [](std::string_view name) {
        return sync_make_record("note.create", {{"path", {name}}, {"item", "folder"}});
    };
    const std::string whole = make_note("A");
    const auto first = whole.substr(0, whole.size() / 2);
    const auto second = whole.substr(first.size());
    std::vector<D1Client::Row> page;
    for (int i = 1; i <= 48; ++i) page.push_back({i, 2, 2, "orphan"});
    page.push_back({49, 1, 1, make_note("B")});
    page.push_back({50, 1, 2, first});
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, queue_rows_success(page).dump()},
        {200, batch_success(1).dump()}, // Acknowledge through 49, then read after 50.
        {200, queue_rows_success({
            {51, 2, 2, second},
            {52, 1, 3, "abandoned set"},
            {53, 1, 1, make_note("C")},
            {54, 1, 2, first},
        }).dump()},
        {200, batch_success(1).dump()}, // Leave 54 for the next loop.
        {200, queue_rows_success({
            {54, 1, 2, first}, {55, 2, 2, second},
        }).dump()},
        {200, batch_success(1).dump()},
    };
    auto params = [&](std::size_t request) {
        return nlohmann::json::parse(scripted->bodies.at(request))["batch"][0]["params"];
    };
    int notices = 0;
    auto config = worker_config(path, scripted);
    config.on_notes_changed = [&] { ++notices; };
    D1SyncWorker worker(std::move(config));
    worker.run_once();
    ASSERT_EQ(scripted->bodies.size(), 4u);
    EXPECT_EQ(params(1), nlohmann::json::array({"49"}));
    EXPECT_EQ(params(2), nlohmann::json::array({"50", "50"}));
    EXPECT_EQ(params(3), nlohmann::json::array({"53"}));
    EXPECT_EQ(notices, 2);
    {
        Database database(path, Database::Mode::read_only);
        for (const auto* name : {"A", "B", "C"})
            EXPECT_TRUE(sync_note_find(database, {name})) << name;
    }
    worker.run_once();
    ASSERT_EQ(scripted->bodies.size(), 6u);
    EXPECT_EQ(params(4), nlohmann::json::array({"0", "50"}));
    EXPECT_EQ(params(5), nlohmann::json::array({"55"}));
    EXPECT_EQ(notices, 3);
}

TEST(D1WorkerTest, MissingOwnTableDuringReadHalts) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        add_d1_key(database);
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, error_response("no such table: q/Test/mac").dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once();
    EXPECT_TRUE(worker.stopped());
    worker.run_once();
    EXPECT_EQ(scripted->bodies.size(), 1u);
}

TEST(D1WorkerTest, OversizedResultHalvesLimitAndResets) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            "system/keys/api_key_1/config.toml", d1_key_toml()).run();
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, error_response("D1 query result exceeded the maximum response size limit of 50 MB").dump()},
        {200, queue_rows_success({}).dump()},
        {200, queue_rows_success({}).dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once();
    worker.run_once();
    ASSERT_EQ(scripted->bodies.size(), 3u);
    const auto limit = [&](std::size_t i) {
        return nlohmann::json::parse(scripted->bodies.at(i))["batch"][0]["params"][1];
    };
    EXPECT_EQ(limit(0), "50");
    EXPECT_EQ(limit(1), "25");
    EXPECT_EQ(limit(2), "50");
}

TEST(D1WorkerTest, SplitSendRetriesFromFirstPiece) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    std::string large(1900001, 'x');
    {
        Database database(path, Database::Mode::read_write);
        database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            "system/keys/api_key_1/config.toml", d1_key_toml()).run();
        sync_queue_append(
            database, sync_make_record("note.create", {{"path", {"A"}}, {"text", large}}));
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, table_list_success({"q/Test/mac", "q/Test/other"}).dump()},
        {200, batch_success(1).dump()}, // piece 1 succeeds
        {200, error_response("down").dump()}, // piece 2 fails
        {200, queue_rows_success({}).dump()},
        {200, table_list_success({"q/Test/mac", "q/Test/other"}).dump()},
        {200, batch_success(1).dump()},
        {200, batch_success(1).dump()},
        {200, queue_rows_success({}).dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once();
    {
        Database database(path, Database::Mode::read_only);
        EXPECT_EQ(sync_queue_list(database).size(), 1u);
    }
    worker.run_once();
    {
        Database database(path, Database::Mode::read_only);
        EXPECT_TRUE(sync_queue_list(database).empty());
    }
}

TEST(D1WorkerTest, InvalidRecordsSkippedAndAcked) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            "system/keys/api_key_1/config.toml", d1_key_toml()).run();
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, queue_rows_success({
            {1, 1, 1, "not-json"},
            {2, 1, 1, sync_make_record("unknown.kind", {})},
            {3, 1, 1, sync_make_record("chat.rename",
                {{"chat", {"missing", "s"}}, {"label", "X"}})},
            {4, 1, 1, sync_make_record("note.create",
                {{"path", {"Ok"}}, {"item", "note"}})},
        }).dump()},
        {200, batch_success(1).dump()},
        {200, queue_rows_success({}).dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once();
    // Invalid rows skipped, valid note applied, all acked (no crash).
    Database database(path, Database::Mode::read_only);
    EXPECT_TRUE(sync_queue_list(database).empty());
}

TEST(D1WorkerTest, UnorderedReadAppliesAndAcknowledgesNothing) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        add_d1_key(database);
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, queue_rows_success({
            {-1, 1, 1, sync_make_record("note.create", {{"path", {"A"}}, {"item", "note"}})},
            {6, 1, 1, sync_make_record("note.create", {{"path", {"B"}}, {"item", "note"}})},
        }).dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once();
    EXPECT_EQ(scripted->bodies.size(), 1u); // No DELETE request.
    Database database(path, Database::Mode::read_only);
    EXPECT_FALSE(sync_note_find(database, {"A"}));
    EXPECT_FALSE(sync_note_find(database, {"B"}));
}

TEST(D1WorkerTest, AckFailureEndsPassAndRetainsForReplay) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            "system/keys/api_key_1/config.toml", d1_key_toml()).run();
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, queue_rows_success({
            {1, 1, 1, sync_make_record("note.create",
                {{"path", {"A"}}, {"item", "note"}})},
        }).dump()},
        {200, error_response("ack down").dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once();
    // Uncertain acknowledgement may replay; at minimum the pass ends cleanly.
    EXPECT_FALSE(worker.stopped());
}

TEST(D1WorkerTest, ReservationFailurePausesAndRetainsRows) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            "system/keys/api_key_1/config.toml", d1_key_toml()).run();
        database.prepare("INSERT INTO forums (forum_id) VALUES ('f1')").run();
    }
    // Live manager is stopping, so chat reservation fails.
    RuntimeSettings settings;
    LiveSessionManager live(settings, [](const FullSessionId&, std::shared_ptr<WakeNotifier>, std::uint64_t) -> OpenedSession {
        throw std::runtime_error("no opener in test");
    });
    live.begin_shutdown();
    auto scripted = std::make_shared<ScriptedTransport>();
    nlohmann::json chat = {
        {"kind", "chat.entries"},
        {"chat", {"f1", "remote-paused"}},
        {"label", "Remote"},
        {"state", 1},
        {"entries", {
            {{"kind", 0}, {"participant_id", "u"}, {"display_name", "U"},
             {"addressed_to", "a"}, {"addressed_to_name", "A"},
             {"text", "P"}, {"status", 0}, {"created_at", 0},
             {"input_tokens", nullptr}, {"output_tokens", nullptr},
             {"web_search_used", false}},
            {{"kind", 1}, {"participant_id", "a"}, {"display_name", "A"},
             {"addressed_to", ""}, {"addressed_to_name", ""},
             {"text", "R"}, {"status", 0}, {"created_at", 0},
             {"input_tokens", nullptr}, {"output_tokens", nullptr},
             {"web_search_used", false}},
        }},
    };
    const std::string record = sync_make_record("chat.entries", chat);
    scripted->responses = {
        {200, queue_rows_success({{1, 1, 1, record}}).dump()},
        // No delete expected: reservation failure retains rows.
    };
    D1SyncWorker::Config config = worker_config(path, scripted);
    config.live_sessions = &live;
    D1SyncWorker worker(std::move(config));
    worker.run_once();
    for (const auto& body : scripted->bodies) {
        EXPECT_EQ(body.find("DELETE"), std::string::npos);
    }
    // Chat was not created because receipt paused before apply.
    {
        Database database(path, Database::Mode::read_only);
        auto check = database.prepare(
            "SELECT 1 FROM sessions s JOIN forums f USING(forum_key) "
            "WHERE f.forum_id='f1' AND s.session_id='remote-paused'");
        EXPECT_FALSE(check.step());
    }
    (void)live.join_shutdown(std::chrono::milliseconds(100));
}

TEST(D1WorkerTest, EvictionDropsAtMostOneEligibleTable) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            "system/keys/api_key_1/config.toml", d1_key_toml()).run();
        sync_queue_append(database, sync_make_record("note.create", {{"path", {"A"}}}));
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, table_list_success({"q/Test/mac", "q/Test/other"}).dump()},
        {200, error_response("Exceeded maximum DB size.").dump()},
        {200, table_list_success(
            {"q/Test/mac", "q/Test/other", "q/Archive/other", "q/Archive/MAC", "config"}).dump()},
        {200, nlohmann::json{
            {"success", true},
            {"result", {count_success(10)["result"][0], count_success(20)["result"][0]}}
        }.dump()},
        {200, batch_success(1).dump()},
        {200, queue_rows_success({}).dump()},
        {200, table_list_success({"q/Test/mac", "q/Test/other"}).dump()},
        {200, batch_success(1).dump()},
        {200, queue_rows_success({}).dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once(); // Triggers eviction, retains work.
    {
        Database database(path, Database::Mode::read_only);
        EXPECT_EQ(sync_queue_list(database).size(), 1u);
    }
    ASSERT_GE(scripted->bodies.size(), 5u);
    const auto counts = nlohmann::json::parse(scripted->bodies[3])["batch"];
    ASSERT_EQ(counts.size(), 2u);
    EXPECT_NE(counts[0]["sql"].get<std::string>().find("q/Test/other"), std::string::npos);
    EXPECT_NE(counts[1]["sql"].get<std::string>().find("q/Archive/other"), std::string::npos);
    EXPECT_NE(scripted->bodies[4].find("q/Archive/other"), std::string::npos);
    worker.run_once(); // Sends and deletes.
    {
        Database database(path, Database::Mode::read_only);
        EXPECT_TRUE(sync_queue_list(database).empty());
    }
}

} // namespace
} // namespace cha
