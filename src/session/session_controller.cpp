#include "session/session_controller.h"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace cha {
namespace {

void merge_update(SessionUpdate& all, SessionUpdate one) {
    all.render_needed = all.render_needed || one.render_needed;
    all.end_session = all.end_session || one.end_session;
    all.clear_input = all.clear_input || one.clear_input;
    if (one.notice) {
        all.notice = std::move(one.notice);
    }
}

std::string format_handle_notice(
    std::string_view handle,
    const HandleResolution& resolution,
    const AgentRoster& roster) {
    if (resolution.match == HandleMatch::unknown) {
        return "Unknown agent @" + std::string(handle)
            + ". Agents in this room: " + roster.handle_list();
    }
    std::string result =
        "Ambiguous agent @" + std::string(handle) + ": matches ";
    for (std::size_t i = 0; i < resolution.candidates.size(); ++i) {
        if (i) {
            result += ", ";
        }
        result += "@" + resolution.candidates[i]->name;
    }
    return result + ". Type more of the name.";
}

std::string format_roster_notice(
    const AgentRoster& roster,
    const ParticipantId& default_agent_id) {
    std::ostringstream result;
    result << "Agents in this room (" << roster.agents().size()
           << "), * marks the default.";
    result << " Any unambiguous prefix works.";
    for (const AgentInfo& agent : roster.agents()) {
        result << " | " << (agent.id == default_agent_id ? "* " : "")
               << "@" << agent.name << "  " << agent.model << "  "
               << agent.api << "  "
               << (agent.streaming ? "streaming" : "non-streaming");
    }
    return result.str();
}

std::string format_session_information(
    const Conversation& conversation,
    const AgentRoster& roster,
    const ParticipantId& default_agent_id) {
    std::ostringstream text;
    text << "Transcript entries: " << conversation.snapshot().entries.size()
         << " | " << format_roster_notice(roster, default_agent_id);
    return text.str();
}

} // namespace

std::unique_ptr<SessionController> SessionController::from_definitions(
    std::vector<AgentDefinition> definitions,
    std::filesystem::path database_path,
    ConversationRestore restored) {
    return std::unique_ptr<SessionController>(new SessionController(
        std::move(definitions),
        std::move(database_path),
        std::move(restored)));
}

std::unique_ptr<SessionController> SessionController::from_backends_for_testing(
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    std::filesystem::path database_path,
    ConversationRestore restored) {
    return std::unique_ptr<SessionController>(new SessionController(
        std::move(backends),
        std::move(database_path),
        std::move(restored)));
}

SessionController::SessionController(
    std::vector<AgentDefinition> definitions,
    std::filesystem::path path,
    ConversationRestore restored)
    : journal_(std::move(path)),
      registry_(conversation_, std::move(definitions)),
      response_(conversation_, journal_, registry_),
      default_agent_id_(registry_.roster().first().id) {
    initialize(std::move(restored));
}

SessionController::SessionController(
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    std::filesystem::path path,
    ConversationRestore restored)
    : journal_(std::move(path)),
      registry_(conversation_, std::move(backends)),
      response_(conversation_, journal_, registry_),
      default_agent_id_(registry_.roster().first().id) {
    initialize(std::move(restored));
}

SessionController::~SessionController() {
    try {
        shutdown();
    } catch (...) {
    }
}

void SessionController::initialize(ConversationRestore restored) {
    response_.restore(std::move(restored));
}

void SessionController::merge_response(SessionUpdate& update, ResponseUpdate response) {
    update.render_needed = update.render_needed || response.render_needed;
    if (response.notice) {
        update.notice = std::move(response.notice);
    }
}

SessionUpdate SessionController::busy_notice() const {
    return {.notice = std::string(generation_in_progress_notice)};
}

SessionUpdate SessionController::submit_prompt(
    std::string text,
    std::string handle) {
    if (response_.generation_status().active) {
        return busy_notice();
    }
    if (text.empty() && handle.empty()) {
        return {};
    }

    SessionUpdate update;
    const AgentInfo* target = nullptr;
    if (handle.empty()) {
        target = roster().find(default_agent_id_);
    } else {
        const HandleResolution resolution = roster().resolve_handle(handle);
        if (resolution.match != HandleMatch::resolved) {
            update.notice = format_handle_notice(handle, resolution, roster());
            return update;
        }
        target = resolution.agent;
    }
    if (!target) {
        throw std::logic_error("Default agent is not in the current roster");
    }
    if (text.empty()) {
        update.notice = "Prompt for @" + target->name + " is empty";
        return update;
    }

    update.clear_input = true;
    merge_response(update, response_.start(std::move(text), *target));
    return update;
}

SessionUpdate SessionController::clear_conversation() {
    if (response_.generation_status().active) {
        return busy_notice();
    }
    try {
        journal_.clear();
    } catch (const std::exception& error) {
        throw std::runtime_error(
            std::string("Failed to persist /clear: ") + error.what());
    }
    conversation_.clear();
    return {
        .render_needed = true,
        .clear_input = true,
        .notice = "Conversation cleared",
    };
}

SessionUpdate SessionController::session_information() {
    if (response_.generation_status().active) {
        return busy_notice();
    }
    return {
        .render_needed = true,
        .clear_input = true,
        .notice = format_session_information(
            conversation_, roster(), default_agent_id_),
    };
}

SessionUpdate SessionController::agent_information() {
    if (response_.generation_status().active) {
        return busy_notice();
    }
    return {
        .render_needed = true,
        .clear_input = true,
        .notice = format_roster_notice(roster(), default_agent_id_),
    };
}

SessionUpdate SessionController::set_default_agent(std::string_view handle) {
    if (response_.generation_status().active) {
        return busy_notice();
    }
    SessionUpdate update{.clear_input = true};
    if (handle.empty()) {
        update.notice = "Usage: /@AgentName";
        return update;
    }
    const HandleResolution result = roster().resolve_handle(handle);
    if (result.match != HandleMatch::resolved) {
        update.notice = format_handle_notice(handle, result, roster());
        return update;
    }
    default_agent_id_ = result.agent->id;
    update.notice = "Default agent is now " + result.agent->name;
    return update;
}

SessionUpdate SessionController::request_stop() {
    SessionUpdate update;
    if (!response_.generation_status().active) {
        update.notice = "No generation is active";
        return update;
    }
    registry_.cancel();
    update.notice = "Stopping generation...";
    return update;
}

SessionUpdate SessionController::handle_agent_event(AgentEvent event) {
    SessionUpdate update;
    merge_response(update, response_.apply(std::move(event)));
    return update;
}

SessionUpdate SessionController::receive() {
    SessionUpdate update;
    AgentEvent event = AgentCompleted{};
    while (true) {
        const ChannelReadStatus status = registry_.try_receive(event);
        if (status == ChannelReadStatus::empty) {
            break;
        }
        if (status == ChannelReadStatus::closed) {
            update.end_session = true;
            break;
        }
        merge_update(update, handle_agent_event(std::move(event)));
    }
    return update;
}

void SessionController::shutdown() {
    if (shutdown_) {
        return;
    }
    shutdown_ = true;
    registry_.stop();
    (void)receive();
}

} // namespace cha
