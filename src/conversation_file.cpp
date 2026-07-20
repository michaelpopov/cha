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

constexpr int conversation_file_version = 4;

// Retains the target identity needed to materialize an interrupted turn as a typed error.
struct PendingTurn {
    RequestId request_id{};
    ParticipantId agent_id;
};

enum class TurnRecordKind {
    started,
    completed,
    cancelled,
    failed,
};

void validate_turn_entry(
    TurnRecordKind record_kind,
    RequestId request_id,
    const ConversationEntry& entry) {

    EntryKind expected_kind = EntryKind::human;
    CompletionStatus expected_status = CompletionStatus::complete;
    switch (record_kind) {
    case TurnRecordKind::started:
        break;
    case TurnRecordKind::completed:
        expected_kind = EntryKind::agent;
        break;
    case TurnRecordKind::cancelled:
        expected_kind = EntryKind::agent;
        expected_status = CompletionStatus::cancelled;
        break;
    case TurnRecordKind::failed:
        expected_kind = EntryKind::error;
        expected_status = CompletionStatus::failed;
        break;
    }

    if (request_id == 0 || entry.kind != expected_kind || entry.status != expected_status
        || !entry.request_id || *entry.request_id != request_id) {
        throw std::invalid_argument("Turn entry does not match its record type and request ID");
    }
}

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

std::string_view kind_name(EntryKind kind) {
    switch (kind) {
    case EntryKind::human:
        return "human";
    case EntryKind::agent:
        return "agent";
    case EntryKind::notice:
        return "notice";
    case EntryKind::error:
        return "error";
    }
    throw std::logic_error("Unknown conversation entry kind");
}

std::string_view status_name(CompletionStatus status) {
    switch (status) {
    case CompletionStatus::complete:
        return "complete";
    case CompletionStatus::streaming:
        return "streaming";
    case CompletionStatus::cancelled:
        return "cancelled";
    case CompletionStatus::failed:
        return "failed";
    }
    throw std::logic_error("Unknown completion status");
}

EntryKind parse_kind(const std::filesystem::path& path, std::size_t line, std::string_view name) {
    if (name == "human") {
        return EntryKind::human;
    }
    if (name == "agent") {
        return EntryKind::agent;
    }
    if (name == "notice") {
        return EntryKind::notice;
    }
    if (name == "error") {
        return EntryKind::error;
    }
    invalid_file(path, line, "unknown entry kind");
}

CompletionStatus parse_status(const std::filesystem::path& path, std::size_t line, std::string_view name) {
    if (name == "complete") {
        return CompletionStatus::complete;
    }
    if (name == "streaming") {
        return CompletionStatus::streaming;
    }
    if (name == "cancelled") {
        return CompletionStatus::cancelled;
    }
    if (name == "failed") {
        return CompletionStatus::failed;
    }
    invalid_file(path, line, "unknown completion status");
}

Json entry_json(const ConversationEntry& entry) {
    require_terminal_conversation_entry(entry);
    Json result{
        {"id", entry.id},
        {"kind", kind_name(entry.kind)},
        {"participant_id", entry.participant_id},
        {"display_name", entry.display_name},
        {"text", entry.text},
        {"status", status_name(entry.status)},
    };
    if (entry.request_id) {
        result["request_id"] = *entry.request_id;
    }
    return result;
}

ConversationEntry parse_entry(
    const std::filesystem::path& path,
    std::size_t line,
    const Json& value) {
    if (!value.is_object()
        || !value.contains("id") || !value.at("id").is_number_unsigned()
        || !value.contains("kind") || !value.at("kind").is_string()
        || !value.contains("participant_id") || !value.at("participant_id").is_string()
        || !value.contains("display_name") || !value.at("display_name").is_string()
        || !value.contains("text") || !value.at("text").is_string()
        || !value.contains("status") || !value.at("status").is_string()
        || (value.contains("request_id") && !value.at("request_id").is_number_unsigned())) {
        invalid_file(path, line, "invalid typed conversation entry");
    }

    ConversationEntry entry{
        .id = value.at("id").get<EntryId>(),
        .kind = parse_kind(path, line, value.at("kind").get<std::string>()),
        .participant_id = value.at("participant_id").get<std::string>(),
        .display_name = value.at("display_name").get<std::string>(),
        .text = value.at("text").get<std::string>(),
        .status = parse_status(path, line, value.at("status").get<std::string>()),
    };
    if (value.contains("request_id")) {
        entry.request_id = value.at("request_id").get<RequestId>();
    }
    try {
        require_terminal_conversation_entry(entry);
    } catch (const std::invalid_argument&) {
        invalid_file(path, line, "invalid typed conversation entry values");
    }
    return entry;
}

void initialize_file(const std::filesystem::path& path) {
    try {
        create_conversation_file_exclusive(path);
    } catch (const std::system_error& error) {
        if (error.code() != std::errc::file_exists) {
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
    if (header.value("type", "") != "conversation"
        || header.value("version", 0) != conversation_file_version) {
        invalid_file(path, 1, "unsupported header");
    }
}

void track_entry(
    const std::filesystem::path& path,
    std::size_t line,
    ConversationRestore& result,
    EntryId& last_entry_id,
    ConversationEntry entry) {
    if (entry.id <= last_entry_id) {
        invalid_file(path, line, "conversation entry IDs must be strictly increasing");
    }
    last_entry_id = entry.id;
    result.next_entry_id = std::max(result.next_entry_id, entry.id + 1);
    result.entries.push_back(std::move(entry));
}

} // namespace

void create_conversation_file_exclusive(const std::filesystem::path& path) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    const std::string header = Json{{"type", "conversation"}, {"version", conversation_file_version}}.dump() + '\n';
    append_bytes(path, header, O_CREAT | O_EXCL);
}

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
    std::optional<PendingTurn> pending;
    EntryId last_entry_id = 0;
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

        const std::string type = record.value("type", "");
        if (type == "clear") {
            result.entries.clear();
            pending.reset();
            continue;
        }
        if (type == "entry") {
            if (!record.contains("entry")) {
                invalid_file(path, line_number, "entry record has no entry");
            }
            track_entry(
                path,
                line_number,
                result,
                last_entry_id,
                parse_entry(path, line_number, record.at("entry")));
            continue;
        }
        if (type == "turn_started") {
            if (pending) {
                invalid_file(path, line_number, "a previous turn has no terminal record");
            }
            if (!record.contains("request_id") || !record.at("request_id").is_number_unsigned()
                || !record.contains("agent_id") || !record.at("agent_id").is_string()
                || record.at("agent_id").get_ref<const std::string&>().empty()
                || !record.contains("prompt")) {
                invalid_file(path, line_number, "invalid turn_started record");
            }
            const RequestId request_id = record.at("request_id").get<RequestId>();
            ConversationEntry prompt = parse_entry(path, line_number, record.at("prompt"));
            try {
                validate_turn_entry(TurnRecordKind::started, request_id, prompt);
            } catch (const std::invalid_argument& error) {
                invalid_file(path, line_number, error.what());
            }
            pending = PendingTurn{request_id, record.at("agent_id").get<std::string>()};
            result.next_request_id = std::max(result.next_request_id, request_id + 1);
            track_entry(path, line_number, result, last_entry_id, std::move(prompt));
            continue;
        }
        if (type == "turn_completed" || type == "turn_cancelled" || type == "turn_failed") {
            if (!record.contains("request_id") || !record.at("request_id").is_number_unsigned()) {
                invalid_file(path, line_number, "terminal turn record has no request ID");
            }
            const RequestId request_id = record.at("request_id").get<RequestId>();
            result.next_request_id = std::max(result.next_request_id, request_id + 1);
            if (!pending || pending->request_id != request_id) {
                invalid_file(path, line_number, "terminal turn record does not match the active request");
            }

            const char* field = type == "turn_failed" ? "error" : "response";
            const bool response_optional = type == "turn_cancelled";
            if (!response_optional && !record.contains(field)) {
                invalid_file(path, line_number, "terminal turn record has no typed entry");
            }
            if (response_optional && !record.contains(field)) {
                pending.reset();
                continue;
            }
            ConversationEntry entry = parse_entry(path, line_number, record.at(field));
            const TurnRecordKind record_kind =
                type == "turn_completed" ? TurnRecordKind::completed
                : type == "turn_cancelled" ? TurnRecordKind::cancelled
                                           : TurnRecordKind::failed;
            try {
                validate_turn_entry(record_kind, request_id, entry);
            } catch (const std::invalid_argument& error) {
                invalid_file(path, line_number, error.what());
            }
            track_entry(path, line_number, result, last_entry_id, std::move(entry));
            pending.reset();
            continue;
        }

        invalid_file(path, line_number, "unknown record type");
    }

    if (line_number == 0) {
        invalid_file(path, 1, "missing header");
    }

    if (pending) {
        ConversationEntry error = make_error_entry(
            result.next_entry_id++,
            "Response interrupted before completion",
            pending->request_id,
            pending->agent_id);
        result.entries.push_back(error);
        result.interrupted_turns.push_back({pending->request_id, std::move(error)});
    }

    return result;
}

std::vector<ConversationEntry> load_conversation_file(const std::filesystem::path& path) {
    return load_conversation_state(path).entries;
}

void save_conversation_file(const std::filesystem::path& path, const Conversation& conversation) {
    const ConversationSnapshot snapshot = conversation.snapshot();
    if (snapshot.open_entry_id) {
        throw std::logic_error("Cannot save a conversation with a streaming entry");
    }

    const std::filesystem::path temporary = path.string() + ".tmp-" + std::to_string(::getpid());
    std::string contents = Json{{"type", "conversation"}, {"version", conversation_file_version}}.dump() + '\n';
    for (const ConversationEntry& entry : snapshot.entries) {
        contents += Json{{"type", "entry"}, {"entry", entry_json(entry)}}.dump();
        contents.push_back('\n');
    }
    append_bytes(temporary, contents, O_CREAT | O_TRUNC);
    std::filesystem::rename(temporary, path);
}

ConversationJournal::ConversationJournal(std::filesystem::path path) : path_(std::move(path)) {
    prepare_conversation_file(path_);
}

void ConversationJournal::append(const ConversationEntry& entry) {
    std::lock_guard lock(mutex_);
    append_record(path_, Json{{"type", "entry"}, {"entry", entry_json(entry)}});
}

void ConversationJournal::start_turn(
    RequestId request_id,
    std::string_view agent_id,
    const ConversationEntry& prompt) {
    if (agent_id.empty()) {
        throw std::invalid_argument("A turn requires a non-empty agent identity");
    }
    validate_turn_entry(TurnRecordKind::started, request_id, prompt);
    std::lock_guard lock(mutex_);
    append_record(path_, Json{
        {"type", "turn_started"},
        {"request_id", request_id},
        {"agent_id", agent_id},
        {"prompt", entry_json(prompt)},
    });
}

void ConversationJournal::complete_turn(RequestId request_id, const ConversationEntry& response) {
    validate_turn_entry(TurnRecordKind::completed, request_id, response);
    std::lock_guard lock(mutex_);
    append_record(path_, Json{
        {"type", "turn_completed"},
        {"request_id", request_id},
        {"response", entry_json(response)},
    });
}

void ConversationJournal::cancel_turn(
    RequestId request_id,
    std::optional<ConversationEntry> response) {

    if (request_id == 0) {
        throw std::invalid_argument("A cancelled turn requires a positive request ID");
    }
    if (response) {
        validate_turn_entry(TurnRecordKind::cancelled, request_id, *response);
    }
    std::lock_guard lock(mutex_);
    Json record{
        {"type", "turn_cancelled"},
        {"request_id", request_id},
    };
    if (response) {
        record["response"] = entry_json(*response);
    }
    append_record(path_, record);
}

void ConversationJournal::fail_turn(RequestId request_id, const ConversationEntry& error) {
    validate_turn_entry(TurnRecordKind::failed, request_id, error);
    std::lock_guard lock(mutex_);
    append_record(path_, Json{
        {"type", "turn_failed"},
        {"request_id", request_id},
        {"error", entry_json(error)},
    });
}

void ConversationJournal::clear() {
    std::lock_guard lock(mutex_);
    append_record(path_, Json{{"type", "clear"}});
}

} // namespace cha
