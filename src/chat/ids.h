#pragma once

#include <string>

namespace cha {

// Domain-specific names make declarations self-describing while keeping
// parsing, filesystem, and protocol boundaries inexpensive. These aliases can
// become strong value types later without first having to discover which
// strings carry which identity.
using ForumId = std::string;
using CharacterId = std::string;
using SessionId = std::string;

} // namespace cha
