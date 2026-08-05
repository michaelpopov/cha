#pragma once
#include "application/application_result.h"
#include "application/environment.h"
#include <memory>
#include <string>
#include <string_view>
namespace cha {
class WakeNotifier;
// Owner-thread coordinator for application-wide persona and session navigation.
class ChatApplication {
public:
    ChatApplication(const Workspace& workspace, WakeNotifier& notifier);
    ~ChatApplication();
    ChatApplication(const ChatApplication&) = delete;
    ChatApplication& operator=(const ChatApplication&) = delete;
    SessionController& controller() noexcept;
    // Explicitly ends the current controller so frontends can propagate a
    // clean-path persistence failure rather than relying on destructor cleanup.
    void shutdown();
    const SessionDescriptor& descriptor() const noexcept;
    std::string_view selected_persona() const noexcept { return selected_persona_; }
    std::string_view selected_author_key() const noexcept { return selected_author_key_; }
    ApplicationResult iam(std::string_view name);
    ApplicationResult forums() const;
    ApplicationResult personas() const;
    ApplicationResult sessions(std::string_view forum);
    ApplicationResult members(std::string_view forum) const;
    ApplicationResult open(std::string_view forum, std::string_view session);
    ApplicationResult create(std::string forum, std::string session);
private:
    ApplicationResult switch_to(OpenedSession prepared, bool consumed = true);
    ApplicationResult error(std::string message, bool consumed = true) const;
    ApplicationEnvironment environment_;
    WakeNotifier& notifier_;
    std::string selected_persona_;
    std::string selected_author_key_;
    OpenedSession current_;
};
} // namespace cha
