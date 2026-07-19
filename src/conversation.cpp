#include "conversation.h"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace cha {

Conversation::Conversation(Conversation&& other) noexcept {
    std::lock_guard lock(other.mutex_);
    messages_ = std::move(other.messages_);
    revision_ = other.revision_;
    message_open_ = other.message_open_;
}

Conversation& Conversation::operator=(Conversation&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    std::scoped_lock lock(mutex_, other.mutex_);
    messages_ = std::move(other.messages_);
    revision_ = other.revision_;
    message_open_ = other.message_open_;
    return *this;
}

void Conversation::add_message(std::string author, std::string text) {
    std::lock_guard lock(mutex_);
    if (message_open_) {
        throw std::logic_error("Cannot add a message while another message is open");
    }
    if (author.empty()) {
        throw std::invalid_argument("Conversation message author cannot be empty");
    }

    messages_.push_back(ConversationMessage{std::move(author), std::move(text)});
    ++revision_;
}

void Conversation::begin_message(std::string author) {
    std::lock_guard lock(mutex_);
    if (message_open_) {
        throw std::logic_error("A conversation message is already open");
    }
    if (author.empty()) {
        throw std::invalid_argument("Conversation message author cannot be empty");
    }

    messages_.push_back(ConversationMessage{std::move(author), {}});
    message_open_ = true;
    ++revision_;
}

void Conversation::append_to_message(std::string_view text) {
    std::lock_guard lock(mutex_);
    if (!message_open_) {
        throw std::logic_error("No conversation message is open");
    }

    messages_.back().text.append(text);
    ++revision_;
}

void Conversation::finish_message() {
    std::lock_guard lock(mutex_);
    message_open_ = false;
}

void Conversation::discard_message() {
    std::lock_guard lock(mutex_);
    if (!message_open_) {
        return;
    }

    messages_.pop_back();
    message_open_ = false;
    ++revision_;
}

ConversationSnapshot Conversation::snapshot() const {
    std::lock_guard lock(mutex_);
    return {messages_, revision_, message_open_};
}

std::vector<ConversationMessage> Conversation::messages() const {
    return snapshot().messages;
}

std::size_t Conversation::revision() const {
    std::lock_guard lock(mutex_);
    return revision_;
}

bool Conversation::message_open() const {
    std::lock_guard lock(mutex_);
    return message_open_;
}

} // namespace cha
