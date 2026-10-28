#pragma once

#include "session/generation_status.h"
#include "chat/transcript.h"
#include "chat/session_character_settings.h"

#include <optional>
#include <string_view>

namespace cha {

// The borrowed generation facts a full projection needs.
struct ControllerGenerationView {
    bool active{};
    std::optional<RequestId> request_id;
    std::string_view character_id;
    std::string_view character_display_name;
    ResponsePhase phase{ResponsePhase::waiting};
    const GenerationProgress* progress{};
};

// A borrowed read model of one live controller, sufficient to build a full
// frontend snapshot.
//
// The view may be used only on the controller owner thread and is invalidated
// by the next controller mutation. It must be consumed synchronously: it must
// never be stored in a runtime field, moved into a mailbox, or captured by
// work that can outlive the call that produced it.
struct ControllerView {
    std::string_view default_character_id;
    std::string_view default_persona_id;
    TranscriptView transcript;
    ControllerGenerationView generation;
    const SessionCharacterSettingsMap* character_settings{};
};

} // namespace cha
