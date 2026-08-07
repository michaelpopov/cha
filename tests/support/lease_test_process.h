#pragma once

#include "support/lease_test_protocol.h"

#include <cerrno>
#include <csignal>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

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

enum class CatalogCreateResult {
    succeeded,
    busy,
};

namespace lease_test_detail {

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

inline CatalogCreateResult create_catalog_session(
    const std::filesystem::path& directory,
    const std::string& forum,
    const std::string& name) {
    std::string directory_text = directory.string();
    std::string forum_text = forum;
    std::string name_text = name;
    char operation[] = "create";
    char* arguments[]{const_cast<char*>(CHA_LEASE_TEST_HELPER), operation,
        directory_text.data(), forum_text.data(), name_text.data(), nullptr};
    const int result = lease_test_detail::wait_for_process(
        lease_test_detail::spawn_helper(arguments));
    if (result == catalog_create_succeeded) return CatalogCreateResult::succeeded;
    if (result == catalog_create_busy) return CatalogCreateResult::busy;
    throw std::runtime_error("Lease test helper failed with exit code " + std::to_string(result));
}

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
        const int flags = fcntl(ready_pipe[0], F_GETFD);
        if (flags == -1
            || fcntl(ready_pipe[0], F_SETFD, flags | FD_CLOEXEC) == -1) {
            const int error = errno;
            lease_test_detail::close_descriptor(ready_pipe[0]);
            lease_test_detail::close_descriptor(ready_pipe[1]);
            throw std::system_error(
                error,
                std::generic_category(),
                "Failed to configure lease test helper pipe");
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

class CatalogHolderProcess {
public:
    explicit CatalogHolderProcess(const std::filesystem::path& directory) {
        int ready_pipe[2]{};
        if (pipe(ready_pipe) == -1) throw std::system_error(errno, std::generic_category(), "Failed to create lease test helper pipe");
        const int flags = fcntl(ready_pipe[0], F_GETFD);
        if (flags == -1 || fcntl(ready_pipe[0], F_SETFD, flags | FD_CLOEXEC) == -1) {
            const int error = errno;
            lease_test_detail::close_descriptor(ready_pipe[0]);
            lease_test_detail::close_descriptor(ready_pipe[1]);
            throw std::system_error(error, std::generic_category(), "Failed to configure lease test helper pipe");
        }
        std::string directory_text = directory.string();
        std::string ready_descriptor = std::to_string(ready_pipe[1]);
        char operation[] = "catalog-hold";
        char* arguments[]{const_cast<char*>(CHA_LEASE_TEST_HELPER), operation,
            directory_text.data(), ready_descriptor.data(), nullptr};
        try { process_ = lease_test_detail::spawn_helper(arguments); }
        catch (...) { lease_test_detail::close_descriptor(ready_pipe[0]); lease_test_detail::close_descriptor(ready_pipe[1]); throw; }
        lease_test_detail::close_descriptor(ready_pipe[1]);
        char ready{};
        const ssize_t count = lease_test_detail::read_ready(ready_pipe[0], ready);
        lease_test_detail::close_descriptor(ready_pipe[0]);
        if (count != 1 || ready != lease_holder_ready) {
            const int result = lease_test_detail::wait_for_process(process_);
            process_ = -1;
            throw std::runtime_error("Catalog holder failed with exit code " + std::to_string(result));
        }
    }
    ~CatalogHolderProcess() { terminate(); }
    CatalogHolderProcess(const CatalogHolderProcess&) = delete;
    CatalogHolderProcess& operator=(const CatalogHolderProcess&) = delete;
    void terminate() noexcept {
        if (process_ == -1) return;
        (void)kill(process_, SIGKILL);
        int status{};
        while (waitpid(process_, &status, 0) == -1 && errno == EINTR) {}
        process_ = -1;
    }
private:
    pid_t process_{-1};
};

} // namespace cha::test
