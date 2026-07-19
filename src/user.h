#pragma once

#include <atomic>
#include <string>

namespace cha {

class Conversation;
class Pipe;

// A free function keeps the top-level user workflow stateless and easy to compose in main.
void run_user(
    const std::string& model,
    std::atomic_bool& cancellation,
    Conversation& conversation,
    Pipe& pipe_in,
    Pipe& pipe_out);

} // namespace cha
