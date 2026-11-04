#pragma once

#include "runtime/error_code.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

namespace cha::app {

class ApplicationError : public std::runtime_error {
public:
    explicit ApplicationError(
        ErrorCode code,
        std::string message = {});
    ErrorCode code;
};

// Terminal result for provider tests and OAuth network work. Abandoned replies
// ignore a later completion; the work itself is not rolled back.
class OperationReply {
public:
    struct Failure {
        ErrorCode code{ErrorCode::internal_error};
        std::string message;
    };
    using Result = std::variant<nlohmann::json, Failure>;

    bool complete(nlohmann::json result);
    bool fail(ErrorCode code, std::string message);
    void set_ready_callback(std::function<void()> callback);
    [[nodiscard]] std::optional<Result> peek() const;
    void abandon() const;
    [[nodiscard]] bool abandoned() const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable ready_;
    mutable bool abandoned_{};
    std::optional<Result> result_;
    mutable std::function<void()> ready_callback_;
};

} // namespace cha::app
