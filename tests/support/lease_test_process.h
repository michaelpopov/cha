#pragma once

#include "support/lease_test_protocol.h"

#include <cerrno>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef CHA_LEASE_TEST_HELPER
#error "CHA_LEASE_TEST_HELPER must name the lease test helper executable"
#endif

extern char** environ;

namespace cha::test {

enum class LeaseProbeResult {
    acquired,
    busy,
};

namespace lease_test_detail {

// Descriptors the parent keeps must not reach a helper: an inherited gate
// writer would keep the gate open after the parent releases it.
inline void keep_in_parent(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFD);
    if (flags == -1 || fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == -1) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to configure lease test helper pipe");
    }
}


inline int wait_for_process(pid_t process) {
    int status{};
    while (waitpid(process, &status, 0) == -1) {
        if (errno != EINTR) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "Failed to wait for lease test helper");
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    throw std::runtime_error("Lease test helper ended unexpectedly");
}

inline pid_t spawn_helper(char* const arguments[]) {
    pid_t process{-1};
    const int error = posix_spawn(
        &process,
        CHA_LEASE_TEST_HELPER,
        nullptr,
        nullptr,
        arguments,
        environ);
    if (error != 0) {
        throw std::system_error(
            error,
            std::generic_category(),
            "Failed to start lease test helper");
    }
    return process;
}

inline void close_descriptor(int descriptor) noexcept {
    if (descriptor != -1) {
        (void)close(descriptor);
    }
}

inline ssize_t read_ready(int descriptor, char& ready) {
    while (true) {
        const ssize_t count = read(descriptor, &ready, 1);
        if (count == -1 && errno == EINTR) {
            continue;
        }
        return count;
    }
}

} // namespace lease_test_detail

// Several helper processes creating in one directory from one clock value, so
// every candidate ID collides. Each child reports ready and then blocks until
// the parent releases them all together, which the constructor arranges and
// run() completes.
class CatalogCreationRace {
public:
    CatalogCreationRace(
        const std::filesystem::path& directory,
        const std::string& forum,
        const std::vector<std::string>& labels,
        std::time_t fixed_time) {

        int gate_pipe[2]{};
        if (pipe(gate_pipe) == -1) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "Failed to create lease test helper pipe");
        }
        gate_read_ = gate_pipe[0];
        gate_write_ = gate_pipe[1];
        try {
            // Children inherit only the gate's read end, so releasing the gate
            // is the parent closing the one remaining writer.
            lease_test_detail::keep_in_parent(gate_write_);
            for (const std::string& label : labels) {
                start_child(directory, forum, label, fixed_time);
            }
        } catch (...) {
            release();
            reap_all();
            lease_test_detail::close_descriptor(gate_read_);
            gate_read_ = -1;
            throw;
        }
    }

    ~CatalogCreationRace() {
        release();
        reap_all();
        lease_test_detail::close_descriptor(gate_read_);
        gate_read_ = -1;
    }

    CatalogCreationRace(const CatalogCreationRace&) = delete;
    CatalogCreationRace& operator=(const CatalogCreationRace&) = delete;

    // Releases every waiting child and requires each one to report a published
    // session. A busy or failed creator is a test failure, not a valid result.
    void run() {
        release();
        for (pid_t& process : processes_) {
            const int result = lease_test_detail::wait_for_process(process);
            process = -1;
            if (result != catalog_create_succeeded) {
                throw std::runtime_error(
                    "Racing creator failed with exit code "
                    + std::to_string(result));
            }
        }
    }

private:
    void start_child(
        const std::filesystem::path& directory,
        const std::string& forum,
        const std::string& label,
        std::time_t fixed_time) {

        int ready_pipe[2]{};
        if (pipe(ready_pipe) == -1) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "Failed to create lease test helper pipe");
        }
        try {
            lease_test_detail::keep_in_parent(ready_pipe[0]);
        } catch (...) {
            lease_test_detail::close_descriptor(ready_pipe[0]);
            lease_test_detail::close_descriptor(ready_pipe[1]);
            throw;
        }

        std::string directory_text = directory.string();
        std::string forum_text = forum;
        std::string label_text = label;
        std::string clock_text = std::to_string(static_cast<long long>(fixed_time));
        std::string ready_descriptor = std::to_string(ready_pipe[1]);
        std::string gate_descriptor = std::to_string(gate_read_);
        char operation[] = "race-create";
        char* arguments[]{
            const_cast<char*>(CHA_LEASE_TEST_HELPER),
            operation,
            directory_text.data(),
            forum_text.data(),
            label_text.data(),
            clock_text.data(),
            ready_descriptor.data(),
            gate_descriptor.data(),
            nullptr,
        };
        pid_t process{-1};
        try {
            process = lease_test_detail::spawn_helper(arguments);
        } catch (...) {
            lease_test_detail::close_descriptor(ready_pipe[0]);
            lease_test_detail::close_descriptor(ready_pipe[1]);
            throw;
        }
        processes_.push_back(process);
        lease_test_detail::close_descriptor(ready_pipe[1]);

        char ready{};
        const ssize_t count =
            lease_test_detail::read_ready(ready_pipe[0], ready);
        lease_test_detail::close_descriptor(ready_pipe[0]);
        if (count != 1 || ready != lease_holder_ready) {
            throw std::runtime_error("Racing creator never became ready");
        }
    }

    void release() noexcept {
        lease_test_detail::close_descriptor(gate_write_);
        gate_write_ = -1;
    }

    void reap_all() noexcept {
        for (pid_t& process : processes_) {
            if (process == -1) {
                continue;
            }
            int status{};
            while (waitpid(process, &status, 0) == -1 && errno == EINTR) {
            }
            process = -1;
        }
    }

    int gate_read_{-1};
    int gate_write_{-1};
    std::vector<pid_t> processes_;
};

inline LeaseProbeResult probe_lease(
    const std::filesystem::path& database) {
    std::string database_text = database.string();
    char operation[] = "probe";
    char* arguments[]{
        const_cast<char*>(CHA_LEASE_TEST_HELPER),
        operation,
        database_text.data(),
        nullptr,
    };
    const int result = lease_test_detail::wait_for_process(
        lease_test_detail::spawn_helper(arguments));
    if (result == lease_probe_acquired) {
        return LeaseProbeResult::acquired;
    }
    if (result == lease_probe_busy) {
        return LeaseProbeResult::busy;
    }
    throw std::runtime_error(
        "Lease test helper failed with exit code " + std::to_string(result));
}

class LeaseHolderProcess {
public:
    explicit LeaseHolderProcess(const std::filesystem::path& database) {
        int ready_pipe[2]{};
        if (pipe(ready_pipe) == -1) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "Failed to create lease test helper pipe");
        }
        try {
            lease_test_detail::keep_in_parent(ready_pipe[0]);
        } catch (...) {
            lease_test_detail::close_descriptor(ready_pipe[0]);
            lease_test_detail::close_descriptor(ready_pipe[1]);
            throw;
        }

        std::string database_text = database.string();
        std::string ready_descriptor = std::to_string(ready_pipe[1]);
        char operation[] = "hold";
        char* arguments[]{
            const_cast<char*>(CHA_LEASE_TEST_HELPER),
            operation,
            database_text.data(),
            ready_descriptor.data(),
            nullptr,
        };
        try {
            process_ = lease_test_detail::spawn_helper(arguments);
        } catch (...) {
            lease_test_detail::close_descriptor(ready_pipe[0]);
            lease_test_detail::close_descriptor(ready_pipe[1]);
            throw;
        }
        lease_test_detail::close_descriptor(ready_pipe[1]);

        char ready{};
        const ssize_t count =
            lease_test_detail::read_ready(ready_pipe[0], ready);
        lease_test_detail::close_descriptor(ready_pipe[0]);
        if (count != 1 || ready != lease_holder_ready) {
            const int result = lease_test_detail::wait_for_process(process_);
            process_ = -1;
            throw std::runtime_error(
                "Lease holder failed with exit code " + std::to_string(result));
        }
    }

    ~LeaseHolderProcess() {
        terminate();
    }

    LeaseHolderProcess(const LeaseHolderProcess&) = delete;
    LeaseHolderProcess& operator=(const LeaseHolderProcess&) = delete;

    void terminate() noexcept {
        if (process_ == -1) {
            return;
        }
        (void)kill(process_, SIGKILL);
        int status{};
        while (waitpid(process_, &status, 0) == -1 && errno == EINTR) {
        }
        process_ = -1;
    }

private:
    pid_t process_{-1};
};

} // namespace cha::test
