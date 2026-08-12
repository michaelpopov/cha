#pragma once

#include <stdexcept>

namespace cha {

class SessionDeleteConflictError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace cha
