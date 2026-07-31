#pragma once

#include <stdexcept>

namespace cha {

class ForumNotFoundError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SessionNotFoundError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace cha
