#pragma once

#include <string>
#include <vector>

namespace cha {

// One workspace user. prompt is the verbatim contents of USER.md.
struct User {
    std::string id;
    std::string display_name;
    std::string prompt;
};

// Every workspace user in lexicographic ID order.
using UserRoster = std::vector<User>;

} // namespace cha
