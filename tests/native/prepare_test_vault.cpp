#include "support/test_workspace.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: cha_prepare_test_vault CONFIG_DIR\n";
        return 2;
    }
    const std::filesystem::path destination(argv[1]);
    std::filesystem::create_directories(destination);
    cha::test::TestWorkspace workspace;
    const std::filesystem::path database = destination / "test.sqlite3";
    (void)cha::test::import_test_database(workspace.root(), database);
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
