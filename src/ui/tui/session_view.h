#pragma once

#include "session/generation_status.h"

#include <optional>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

struct TranscriptView;
class InputEditor;

// Presentation-only application output.  It is owned by the TUI state
// machine, never by a transcript or controller.
struct ApplicationOverlay {
    std::string title;
    std::vector<std::string> rows;
    std::size_t first_visible{};
};

// Identifies one UI-independent input action produced by a terminal front end.
enum class SessionInputKind {
    ignored,
    character,
    enter,
    escape,
    interrupt,
    resize,
    page_up,
    page_down,
    left,
    right,
    up,
    down,
    home,
    end,
    erase,
    backspace,
};

// One decoded input action for PersonaSession, carrying the character when the action is a keystroke.
struct SessionInput {
    SessionInputKind kind{SessionInputKind::ignored};
    wchar_t character{};
};

// The seam between a session and the screen. It declares everything PersonaSession needs — read one
// input action, paint the transcript, editor, generation status and notice, scroll, resize — so
// session behavior can be exercised without curses. Tui is the curses implementation; tests
// substitute a recording one.
class SessionView {
public:
    virtual ~SessionView() = default;

    virtual std::optional<SessionInput> read_input() = 0;
    virtual void render(
        TranscriptView transcript,
        const InputEditor& editor,
        const GenerationStatus& status,
        bool show_addressing,
        std::string_view input_target_name,
        std::string_view notice,
        const ApplicationOverlay* overlay) = 0;
    virtual void scroll_up() = 0;
    virtual void scroll_down() = 0;
    virtual void resize() = 0;
    // Called only after a successful controller replacement.
    virtual void reset_session_view() = 0;
};

} // namespace cha
