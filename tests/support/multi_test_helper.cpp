#include "workspace/workspace_config_store.h"
#include "storage/session_repository.h"
#include "storage/sqlite_storage.h"
#include "workspace/builtins.h"
#include "test_transcript.h"

#include <nlohmann/json.hpp>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

// Each helper is a fresh process with its own store, repository and journal.
// JSON lines keep test coordination independent of production protocols.
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    using namespace cha;
    using Json = nlohmann::json;
    std::unique_ptr<WorkspaceConfigStore> store;
    std::unique_ptr<SessionRepository> sessions;
    std::optional<PreparedSession> owned;
    std::unique_ptr<SessionJournal> journal;
    std::unique_ptr<storage::SqliteDatabase> reader;
    std::string line;
    while (std::getline(std::cin, line)) {
        Json result{{"ok", true}};
        try {
            const auto request = Json::parse(line);
            const auto action = request.at("action").get<std::string>();
            if (action == "open") {
                store = WorkspaceConfigStore::open(argv[1]);
                sessions = std::make_unique<SessionRepository>(
                    [&] { return store->snapshot(); }, store->database_path(), store->workspace_path(),
                    store->welcome_path(), TemporarySessionSeed{
                        {std::string(entrance_id), std::string(welcome_id)}, "Welcome"});
                result["main"] = store->is_main();
            } else if (action == "claim") {
                journal.reset();
                owned.reset();
                owned = sessions->prepare({request.at("forum"), request.at("id")});
                journal = std::make_unique<SessionJournal>(owned->database_path, owned->session_key);
            } else if (action == "write") {
                journal->record_entry(make_notice_entry(owned->restore.next_entry_id++, request.at("text")));
            } else if (action == "begin") {
                journal->start_turn(1, test::human_entry(1, {"human", "You"},
                    {"guide", "Guide"}, "Pending prompt", 1));
            } else if (action == "release") {
                journal.reset();
                owned.reset();
            } else if (action == "close") {
                reader.reset();
                journal.reset();
                owned.reset();
                sessions.reset();
                store.reset();
            } else if (action == "settings") {
                (void)store->apply_character_definition("guide", request.at("name").get<std::string>());
            } else if (action == "role") {
                result["main"] = store->is_main();
                result["name"] = store->snapshot()->find_character("guide")->character.display_name;
            } else if (action == "reader") {
                reader = std::make_unique<storage::SqliteDatabase>(store->database_path(),
                    storage::SqliteDatabase::Mode::read_only);
                reader->execute("BEGIN");
                (void)reader->prepare("SELECT COUNT(*) FROM sessions").step();
            } else return 1;
        } catch (const SessionBusyError&) {
            result = {{"ok", false}, {"busy", true}};
        } catch (const SecondaryProcessError&) {
            result = {{"ok", false}, {"secondary", true}};
        } catch (const std::exception& error) {
            result = {{"ok", false}, {"error", error.what()}};
        }
        std::cout << result.dump() << std::endl;
    }
}
