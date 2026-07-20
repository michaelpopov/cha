#pragma once

#include "agent_protocol.h"

namespace cha {

class ChatCoordinator;
class Terminal;

// A free function keeps the top-level user workflow stateless and easy to compose in main.
void run_user(
    Terminal& terminal,
    ChatCoordinator& coordinator,
    AgentEventChannel& events,
    CompletionRequestChannel& requests);

} // namespace cha
