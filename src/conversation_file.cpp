#include "conversation_file.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace cha {
namespace {

using Json = nlohmann::json;

constexpr int conversation_file_version = 3;

[[noreturn]] void invalid_file(const std::filesystem::path& path, std::size_t line, const std::string& reason) {
    throw std::runtime_error(
        "Invalid conversation file '" + path.string() + "' at line " + std::to_string(line) + ": " + reason);
}

void append_bytes(const std::filesystem::path& path, std::string_view bytes, int flags) {
    const int descriptor = ::open(path.c_str(), flags | O_WRONLY | O_CLOEXEC, 0600);
    if (descriptor == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to open conversation file '" + path.string() + "'");
    }

    try {
        while (!bytes.empty()) {
            const ssize_t written = ::write(descriptor, bytes.data(), bytes.size());
            if (written == -1 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                throw std::system_error(errno, std::generic_category(), "Failed to write conversation file '" + path.string() + "'");
            }
            bytes.remove_prefix(static_cast<std::size_t>(written));
        }
        while (::fsync(descriptor) == -1) {
            if (errno != EINTR) {
                throw std::system_error(errno, std::generic_category(), "Failed to sync conversation file '" + path.string() + "'");
            }
        }
    } catch (...) {
        ::close(descriptor);
        throw;
    }
    if (::close(descriptor) == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to close conversation file '" + path.string() + "'");
    }
}

void append_record(const std::filesystem::path& path, const Json& record) {
    const std::string line = record.dump() + '\n';
    append_bytes(path, line, O_APPEND);
}

void initialize_file(const std::filesystem::path& path) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    const std::string header = Json{{"type", "conversation"}, {"version", conversation_file_version}}.dump() + '\n';
    try {
        append_bytes(path, header, O_CREAT | O_EXCL);
    } catch (const std::system_error& error) {
        if (error.code().value() != EEXIST) {
            throw;
        }
    }
}

void validate_header(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to inspect conversation file '" + path.string() + "'");
    }

    std::string header_line;
    if (!std::getline(file, header_line)) {
        throw std::runtime_error("Conversation file '" + path.string() + "' has no complete header");
    }

    Json header;
    try {
        header = Json::parse(header_line);
    } catch (const Json::exception& error) {
        invalid_file(path, 1, error.what());
    }
    const int version = header.value("version", 0);
    if (header.value("type", "") != "conversation" || version != conversation_file_version) {
        invalid_file(path, 1, "unsupported header");
    }
}

} // namespace

void prepare_conversation_file(const std::filesystem::path& path) {
    initialize_file(path);
    const std::uintmax_t size = std::filesystem::file_size(path);
    if (size == 0) {
        const std::string header = Json{{"type", "conversation"}, {"version", conversation_file_version}}.dump() + '\n';
        append_bytes(path, header, O_APPEND);
        return;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to inspect conversation file '" + path.string() + "'");
    }
    file.seekg(-1, std::ios::end);
    char last = '\0';
    file.get(last);
    if (last == '\n') {
        file.close();
        validate_header(path);
        return;
    }

    file.clear();
    file.seekg(0);
    std::string line;
    std::uintmax_t complete_size = 0;
    while (std::getline(file, line)) {
        if (!file.eof()) {
            complete_size += line.size() + 1;
        }
    }
    if (complete_size == 0) {
        throw std::runtime_error("Conversation file '" + path.string() + "' has no complete header");
    }
    file.close();
    std::filesystem::resize_file(path, complete_size);
    validate_header(path);
}

ConversationRestore load_conversation_state(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open conversation file '" + path.string() + "'");
    }

    ConversationRestore result;
    std::optional<RequestId> pending_request;
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
            const int version = record.value("version", 0);
            if (record.value("type", "") != "conversation" || version != conversation_file_version) {
                invalid_file(path, line_number, "unsupported header");
            }
            continue;
        }

        const std::string type = record.value("type", "");
        if (type == "clear") {
            result.messages.clear();
            pending_request.reset();
            continue;
        }

        if (type == "message") {
            if (!record.contains("author") || !record.at("author").is_string()
                || !record.contains("text") || !record.at("text").is_string()) {
                invalid_file(path, line_number, "invalid message record");
            }
            std::string author = record.at("author").get<std::string>();
            if (author.empty()) {
                invalid_file(path, line_number, "message author cannot be empty");
            }
            result.messages.push_back({std::move(author), record.at("text").get<std::string>()});
            continue;
        }

        if (type == "turn_started") {
            if (pending_request) {
                invalid_file(path, line_number, "a previous turn has no terminal record");
            }
            if (!record.contains("request_id") || !record.at("request_id").is_number_unsigned()
                || !record.contains("agent_id") || !record.at("agent_id").is_string()
                || !record.contains("prompt") || !record.at("prompt").is_string()) {
                invalid_file(path, line_number, "invalid turn_started record");
            }
            const RequestId request_id = record.at("request_id").get<RequestId>();
            if (request_id == 0) {
                invalid_file(path, line_number, "request_id must be positive");
            }
            pending_request = request_id;
            result.next_request_id = std::max(result.next_request_id, request_id + 1);
            result.messages.push_back({std::string(user_author), record.at("prompt").get<std::string>()});
            continue;
        }

        if (type == "turn_completed" || type == "turn_cancelled" || type == "turn_failed") {
            if (!record.contains("request_id") || !record.at("request_id").is_number_unsigned()) {
                invalid_file(path, line_number, "terminal turn record has no valid request_id");
            }
            const RequestId request_id = record.at("request_id").get<RequestId>();
            result.next_request_id = std::max(result.next_request_id, request_id + 1);
            if (!pending_request || *pending_request != request_id) {
                invalid_file(path, line_number, "terminal turn record does not match the active request");
            }

            if (type == "turn_failed") {
                if (!record.contains("error") || !record.at("error").is_string()) {
                    invalid_file(path, line_number, "invalid turn_failed record");
                }
                result.messages.push_back({
                    std::string(system_author),
                    "Error: " + record.at("error").get<std::string>(),
                });
            } else {
                const std::string field = type == "turn_completed" ? "response" : "partial_response";
                if (!record.contains("author") || !record.at("author").is_string()
                    || record.at("author").get_ref<const std::string&>().empty()
                    || !record.contains(field) || !record.at(field).is_string()) {
                    invalid_file(path, line_number, "invalid response terminal record");
                }
                result.messages.push_back({
                    record.at("author").get<std::string>(),
                    record.at(field).get<std::string>(),
                });
            }
            pending_request.reset();
            continue;
        }

        invalid_file(path, line_number, "unknown record type");
    }

    if (line_number == 0) {
        invalid_file(path, 1, "missing header");
    }

    if (pending_request) {
        result.interrupted_turns.push_back({*pending_request});
        result.messages.push_back({
            std::string(system_author),
            "Error: Response interrupted before completion",
        });
    }

    return result;
}

std::vector<ConversationMessage> load_conversation_file(const std::filesystem::path& path) {
    return load_conversation_state(path).messages;
}

void save_conversation_file(const std::filesystem::path& path, const Conversation& conversation) {
    const ConversationSnapshot snapshot = conversation.snapshot();
    if (snapshot.message_open) {
        throw std::logic_error("Cannot save a conversation with an open message");
    }

    const std::filesystem::path temporary = path.string() + ".tmp-" + std::to_string(::getpid());
    std::string contents = Json{{"type", "conversation"}, {"version", conversation_file_version}}.dump() + '\n';
    for (const ConversationMessage& message : snapshot.messages) {
        contents += Json{{"type", "message"}, {"author", message.author}, {"text", message.text}}.dump();
        contents.push_back('\n');
    }
    append_bytes(temporary, contents, O_CREAT | O_TRUNC);
    std::filesystem::rename(temporary, path);
}

ConversationJournal::ConversationJournal(std::filesystem::path path) : path_(std::move(path)) {
    prepare_conversation_file(path_);
}

void ConversationJournal::append(const ConversationMessage& message) {
    if (message.author.empty()) {
        throw std::invalid_argument("Conversation message author cannot be empty");
    }
    std::lock_guard lock(mutex_);
    append_record(path_, Json{{"type", "message"}, {"author", message.author}, {"text", message.text}});
}

void ConversationJournal::start_turn(
    RequestId request_id,
    std::string_view agent_id,
    std::string_view prompt) {
    if (request_id == 0 || agent_id.empty()) {
        throw std::invalid_argument("A turn requires a positive request ID and an agent ID");
    }
    std::lock_guard lock(mutex_);
    append_record(path_, Json{
        {"type", "turn_started"},
        {"request_id", request_id},
        {"agent_id", agent_id},
        {"prompt", prompt},
    });
}

void ConversationJournal::complete_turn(
    RequestId request_id,
    std::string_view author,
    std::string_view response) {
    if (request_id == 0 || author.empty()) {
        throw std::invalid_argument("A completed turn requires a request ID and response author");
    }
    std::lock_guard lock(mutex_);
    append_record(path_, Json{
        {"type", "turn_completed"},
        {"request_id", request_id},
        {"author", author},
        {"response", response},
    });
}

void ConversationJournal::cancel_turn(
    RequestId request_id,
    std::string_view author,
    std::string_view partial_response) {
    if (request_id == 0 || author.empty()) {
        throw std::invalid_argument("A cancelled turn requires a request ID and response author");
    }
    std::lock_guard lock(mutex_);
    append_record(path_, Json{
        {"type", "turn_cancelled"},
        {"request_id", request_id},
        {"author", author},
        {"partial_response", partial_response},
    });
}

void ConversationJournal::fail_turn(RequestId request_id, std::string_view error) {
    if (request_id == 0 || error.empty()) {
        throw std::invalid_argument("A failed turn requires a request ID and error");
    }
    std::lock_guard lock(mutex_);
    append_record(path_, Json{
        {"type", "turn_failed"},
        {"request_id", request_id},
        {"error", error},
    });
}

void ConversationJournal::clear() {
    std::lock_guard lock(mutex_);
    append_record(path_, Json{{"type", "clear"}});
}

} // namespace cha
