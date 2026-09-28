#include "workspace/workspace_config_store.h"

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: cha_prepare_linux_config SEED_DIR EXAMPLE_DIR CONFIG_DIR\n";
        return 2;
    }
    try {
        const std::filesystem::path config(argv[3]);
        std::filesystem::create_directory(config);
        std::filesystem::copy_file(
            std::filesystem::path(argv[2]) / "app.toml", config / "app.toml");
        std::filesystem::copy_file(
            std::filesystem::path(argv[2]) / "personal.toml",
            config / "personal.toml");
        (void)cha::import_workspace_configuration(
            argv[1], config / "cha.sqlite3");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Cannot prepare Linux example config: " << error.what() << '\n';
        return 1;
    }
}
