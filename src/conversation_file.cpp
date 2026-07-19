#include "conversation_file.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace cha {
namespace {

using Json = nlohmann::json;

constexpr int conversation_file_version = 1;

[[noreturn]] void invalid_file(const std::filesystem::path& path, std::size_t line, const std::string& reason) {
    throw std::runtime_error(
        "Invalid conversation file '" + path.string() + "' at line " + std::to_string(line) + ": " + reason);
}

} // namespace

std::vector<ConversationMessage> load_conversation_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open conversation file '" + path.string() + "'");
    }

    std::vector<ConversationMessage> messages;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        if (line.empty()) {
            invalid_file(path, line_number, "empty record");
        }

        Json record;
        try {
            record = Json::parse(line);
        } catch (const Json::exception& error) {
            invalid_file(path, line_number, error.what());
        }

        if (line_number == 1) {
            if (record.value("type", "") != "conversation"
                || record.value("version", 0) != conversation_file_version) {
                invalid_file(path, line_number, "unsupported header");
            }
            continue;
        }

        if (record.value("type", "") != "message"
            || !record.contains("author") || !record.at("author").is_string()
            || !record.contains("text") || !record.at("text").is_string()) {
            invalid_file(path, line_number, "invalid message record");
        }
        std::string author = record.at("author").get<std::string>();
        if (author.empty()) {
            invalid_file(path, line_number, "message author cannot be empty");
        }
        messages.push_back({std::move(author), record.at("text").get<std::string>()});
    }

    if (line_number == 0) {
        invalid_file(path, 1, "missing header");
    }

    return messages;
}

void save_conversation_file(const std::filesystem::path& path, const Conversation& conversation) {
    const ConversationSnapshot snapshot = conversation.snapshot();
    if (snapshot.message_open) {
        throw std::logic_error("Cannot save a conversation with an open message");
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("Failed to create conversation file '" + path.string() + "'");
    }

    file << Json{{"type", "conversation"}, {"version", conversation_file_version}}.dump() << '\n';
    for (const ConversationMessage& message : snapshot.messages) {
        file << Json{{"type", "message"}, {"author", message.author}, {"text", message.text}}.dump() << '\n';
    }

    if (!file) {
        throw std::runtime_error("Failed to write conversation file '" + path.string() + "'");
    }
}

} // namespace cha
