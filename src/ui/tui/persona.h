#pragma once

#include <string>

namespace cha {

class ChatApplication;
class Terminal;
class UvEventLoop;

// Runs one application-relative terminal chat.
void run_application(
    Terminal& terminal,
    ChatApplication& application,
    UvEventLoop& event_loop);

} // namespace cha
