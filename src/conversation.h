#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

inline constexpr std::string_view user_author = "You";
inline constexpr std::string_view system_author = "System";

struct ConversationMessage {
    std::string author;
    std::string text;

    bool operator==(const ConversationMessage&) const = default;
};

struct ConversationSnapshot {
    std::vector<ConversationMessage> messages;
    std::size_t revision{};
    bool message_open{};
};

// Keeps the logical conversation independent of whichever file generation currently stores it.
class Conversation {
public:
    Conversation() = default;
    Conversation(const Conversation&) = delete;
    Conversation& operator=(const Conversation&) = delete;
    Conversation(Conversation&&) = delete;
    Conversation& operator=(Conversation&&) = delete;

    void add_message(std::string author, std::string text);
    void begin_message(std::string author);
    void append_to_message(std::string_view text);
    void finish_message();
    void discard_message();
    void replace_messages(std::vector<ConversationMessage> messages);

    [[nodiscard]] ConversationSnapshot snapshot() const;
    [[nodiscard]] std::vector<ConversationMessage> messages() const;
    [[nodiscard]] std::size_t revision() const;
    [[nodiscard]] bool message_open() const;

private:
    mutable std::mutex mutex_;
    std::vector<ConversationMessage> messages_;
    std::size_t revision_{};
    bool message_open_{};
};

} // namespace cha
