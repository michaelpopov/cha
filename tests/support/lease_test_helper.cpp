#include "session/session_catalog.h"
#include "session/session_lease.h"
#include "support/lease_test_protocol.h"

#include <cerrno>
#include <ctime>
#include <filesystem>
#include <optional>
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

// Blocks until the parent releases every child at once by closing the gate's
// write end, so creation attempts collide instead of running in sequence.
bool wait_for_gate(int descriptor) {
    while (true) {
        char ignored{};
        const ssize_t count = read(descriptor, &ignored, 1);
        if (count >= 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
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
    cha::SessionLease lease = cha::SessionLease::acquire(database);
    if (!write_ready(ready_descriptor)) {
        return cha::test::lease_probe_failed;
    }
    (void)close(ready_descriptor);
    while (true) {
        pause();
    }
}

// Creates one session under a caller-supplied clock value, so several helper
// processes released together all derive the same base ID and must resolve the
// collision through candidate leases and atomic publication alone.
int race_create(
    const std::filesystem::path& directory,
    std::string_view forum,
    std::string_view label,
    std::time_t fixed_time,
    int ready_descriptor,
    int gate_descriptor) {

    if (!write_ready(ready_descriptor)) {
        return cha::test::lease_probe_failed;
    }
    (void)close(ready_descriptor);
    if (!wait_for_gate(gate_descriptor)) {
        return cha::test::lease_probe_failed;
    }
    (void)close(gate_descriptor);
    (void)cha::SessionCatalog(
        directory, std::string(forum), [fixed_time] { return fixed_time; })
        .create(std::string(label));
    return cha::test::catalog_create_succeeded;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4 && argc != 8) {
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
            const std::optional<int> ready_descriptor =
                parse_descriptor(argv[3]);
            if (!ready_descriptor) {
                return cha::test::lease_probe_failed;
            }
            return hold_lease(database, *ready_descriptor);
        }
        if (operation == "race-create" && argc == 8) {
            std::size_t parsed{};
            const std::string clock_text(argv[5]);
            const long long fixed_time = std::stoll(clock_text, &parsed);
            const std::optional<int> ready_descriptor =
                parse_descriptor(argv[6]);
            const std::optional<int> gate_descriptor =
                parse_descriptor(argv[7]);
            if (parsed != clock_text.size() || !ready_descriptor
                || !gate_descriptor) {
                return cha::test::lease_probe_failed;
            }
            return race_create(
                database,
                argv[3],
                argv[4],
                static_cast<std::time_t>(fixed_time),
                *ready_descriptor,
                *gate_descriptor);
        }
    } catch (const cha::SessionBusyError&) {
        return cha::test::lease_probe_busy;
    } catch (...) {
        return cha::test::lease_probe_failed;
    }
    return cha::test::lease_probe_failed;
}
