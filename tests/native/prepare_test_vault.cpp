#include "support/test_workspace.h"
#include "storage/folder_store_storage.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr const char usage[] =
    "usage: cha_prepare_test_vault [--provider-port PORT | --folder-stores | --file-references] CONFIG_DIR\n";

// Points the default character at an HTTP provider on 127.0.0.1:PORT and
// turns off session naming, so every provider request is a character reply.
void use_net_provider(const cha::test::TestWorkspace& workspace, int port) {
    const auto session = workspace.root() / "system/session";
    std::filesystem::create_directories(session);
    std::ofstream(session / "config.toml") << "naming_provider = \"test\"\n";
    workspace.write_provider(
        "remote",
        "host = \"127.0.0.1\"\nport = " + std::to_string(port)
            + "\nhttps = false\nmode = \"net\"\nmodel = \"fake\"\n"
              "api = \"chat_completions\"\nstream = false\ntimeout_s = 20\n");
    workspace.write_character_config(
        "display_name = \"Guide\"\nprovider = \"remote\"\n");
    std::ofstream(workspace.root() / "system" / "assistant" / "character.toml")
        << "display_name = \"Assistant\"\nprovider = \"remote\"\n";
}

std::optional<int> parse_port(std::string_view text) {
    int port = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9' || port > 65535) return std::nullopt;
        port = port * 10 + (digit - '0');
    }
    if (port <= 0 || port > 65535) return std::nullopt;
    return port;
}

} // namespace

int main(int argc, char** argv) {
    std::optional<int> provider_port;
    const bool folder_stores = argc == 3 && std::string_view(argv[1]) == "--folder-stores";
    const bool file_references = argc == 3 && std::string_view(argv[1]) == "--file-references";
    if (argc == 4 && std::string_view(argv[1]) == "--provider-port") {
        provider_port = parse_port(argv[2]);
        if (!provider_port) {
            std::cerr << usage;
            return 2;
        }
    } else if (argc != 2 && !folder_stores && !file_references) {
        std::cerr << usage;
        return 2;
    }
    const std::filesystem::path destination(argv[argc - 1]);
    std::filesystem::create_directories(destination);
    cha::test::TestWorkspace workspace;
    if (provider_port) use_net_provider(workspace, *provider_port);
    const std::filesystem::path database = destination / "test.sqlite3";
    (void)cha::test::import_test_database(workspace.root(), database);
    if (file_references) {
        cha::storage::SqliteDatabase db(database, cha::storage::SqliteDatabase::Mode::read_write);
        (void)cha::note_create(db, {}, cha::NoteItemKind::note, "Reference note");
        for (const auto service : {cha::FolderStoreService::openai, cha::FolderStoreService::google}) {
            const auto folder = cha::note_create(db, {}, cha::NoteItemKind::folder,
                service == cha::FolderStoreService::openai ? "OpenAI reference folder" : "Gemini reference folder");
            cha::storage::SqliteTransaction tx(db);
            const auto id = cha::folder_store_register(db, folder.id, service, "missing_native_test_key", 1);
            cha::folder_store_save_remote_id(db, id, "native_reference_fixture_not_a_real_resource");
            cha::folder_store_mark_ready(db, id);
            tx.commit();
        }
    }
    if (folder_stores) {
        cha::storage::SqliteDatabase db(database, cha::storage::SqliteDatabase::Mode::read_write);
        const auto empty = cha::note_create(db, {}, cha::NoteItemKind::folder, "UI store empty");
        const auto failed = cha::note_create(db, {}, cha::NoteItemKind::folder, "UI store failure");
        const auto note = cha::note_create(db, failed.id, cha::NoteItemKind::note, "UI store note");
        (void)cha::note_save(db, note.id, nlohmann::json::parse(
            R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Native store editor fixture"}]}]})"));
        cha::storage::SqliteTransaction tx(db);
        (void)cha::folder_store_register(db, empty.id, cha::FolderStoreService::openai, "missing_native_test_key", 1);
        const auto id = cha::folder_store_register(db, failed.id, cha::FolderStoreService::openai, "missing_native_test_key", 1);
        // Synthetic ownership: the missing key prevents all network requests.
        cha::folder_store_save_remote_id(db, id, "vs_native_ui_fixture_not_a_real_resource");
        tx.commit();
    }
    {
        std::ofstream app(destination / "app.toml");
        app << "vault = \"Test\"\n"
            << "[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    }
    {
        std::ofstream vault(destination / "test.toml");
        vault << "vault_name = \"Test\"\n"
              << "data = " << nlohmann::json(database.string()).dump() << "\n";
    }
    return 0;
}
