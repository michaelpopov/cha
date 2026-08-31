#include "session/session_lease.h"
#include "support/lease_test_protocol.h"

#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <unistd.h>

namespace {

constexpr char test_busy_message[] = "Test database already in use";

bool write_ready(int descriptor) {
    while (true) {
        const ssize_t count =
            write(descriptor, &cha::test::lease_holder_ready, 1);
        if (count == 1) {
            return true;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

std::optional<int> parse_descriptor(std::string_view text) {
    std::size_t parsed{};
    const int descriptor = std::stoi(std::string(text), &parsed);
    if (parsed != text.size() || descriptor < 0) {
        return std::nullopt;
    }
    return descriptor;
}

int hold_lease(const std::filesystem::path& database, int ready_descriptor) {
    cha::SessionLease lease =
        cha::SessionLease::acquire(database, test_busy_message);
    if (!write_ready(ready_descriptor)) {
        return cha::test::lease_probe_failed;
    }
    (void)close(ready_descriptor);
    while (true) {
        pause();
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        return cha::test::lease_probe_failed;
    }
    try {
        const std::string_view operation(argv[1]);
        const std::filesystem::path database(argv[2]);
        if (operation == "probe" && argc == 3) {
            cha::SessionLease lease =
                cha::SessionLease::acquire(database, test_busy_message);
            return cha::test::lease_probe_acquired;
        }
        if (operation == "hold" && argc == 4) {
            const std::optional<int> ready_descriptor =
                parse_descriptor(argv[3]);
            if (!ready_descriptor) {
                return cha::test::lease_probe_failed;
            }
            return hold_lease(database, *ready_descriptor);
        }
    } catch (const cha::SessionBusyError&) {
        return cha::test::lease_probe_busy;
    } catch (...) {
        return cha::test::lease_probe_failed;
    }
    return cha::test::lease_probe_failed;
}
