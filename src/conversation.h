#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

inline constexpr std::string_view user_author = "You";
inline constexpr std::string_view system_author = "System";

// Stores one authored transcript entry for rendering, persistence, and agent context.
struct ConversationMessage {
    std::string author;
    std::string text;

    bool operator==(const ConversationMessage&) const = default;
};

// Provides a consistent read-only copy of conversation state for concurrent consumers.
struct ConversationSnapshot {
    std::vector<ConversationMessage> messages;
    std::size_t revision{};
    bool message_open{};
    std::size_t history_epoch{};
};

// Keeps the thread-safe logical transcript independent of rendering and file storage.
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
    void clear();
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
    std::size_t history_epoch_{};
};

} // namespace cha
