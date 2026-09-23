#include "app/application.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <variant>

// Run in a fresh process: exiting without destructors leaves a real open turn
// for the parent integration test to recover. Never fork a running application.
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    try {
        // The child cannot clean up after _Exit. Keep its runtime directories
        // inside the parent's fixture so the parent removes them after the test.
        const auto temporary = std::filesystem::path(argv[1]) / "interrupted-process";
        std::filesystem::create_directories(temporary);
        if (setenv("TMPDIR", temporary.c_str(), 1) != 0) std::_Exit(2);
        const std::string config = "--config=" + std::string(argv[1]);
        const char* arguments[] = {"itest-helper", config.c_str()};
        auto application = cha::app::Application::open(
            cha::parse_application_command(2, arguments));
        const auto epoch = application->context_epoch();
        const std::string session = argv[2];
        if (!std::holds_alternative<cha::OpenSessionSuccess>(
                application->open_session("lobby", session, epoch))) std::_Exit(3);
        if (!std::holds_alternative<cha::CommandResult>(application->submit(
                "lobby", session, cha::RawCommand{"Interrupted prompt"}, epoch))) {
            std::_Exit(4);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto result = application->snapshot("lobby", session, epoch);
            if (const auto* snapshot = std::get_if<cha::SessionSnapshot>(&result)) {
                if (snapshot->generation.active && !snapshot->transcript.empty()
                    && snapshot->transcript.back().text == argv[3]) {
                    std::_Exit(0);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::_Exit(5);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::_Exit(6);
    }
}
