#include "session/session_lease.h"
#include "support/lease_test_protocol.h"

#include <cerrno>
#include <filesystem>
#include <string>
#include <string_view>

#include <unistd.h>

namespace {

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

int hold_lease(const std::filesystem::path& database, int ready_descriptor) {
    cha::SessionLease lease = cha::SessionLease::acquire(database);
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
            cha::SessionLease lease = cha::SessionLease::acquire(database);
            return cha::test::lease_probe_acquired;
        }
        if (operation == "hold" && argc == 4) {
            std::size_t parsed{};
            const int ready_descriptor = std::stoi(argv[3], &parsed);
            if (parsed != std::string_view(argv[3]).size()
                || ready_descriptor < 0) {
                return cha::test::lease_probe_failed;
            }
            return hold_lease(database, ready_descriptor);
        }
    } catch (const cha::SessionBusyError&) {
        return cha::test::lease_probe_busy;
    } catch (...) {
        return cha::test::lease_probe_failed;
    }
    return cha::test::lease_probe_failed;
}
