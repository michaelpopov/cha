#pragma once

#include "application/generation_status.h"

#include <optional>
#include <string_view>

namespace cha {

class Conversation;
class InputEditor;

// Identifies one UI-independent input action produced by a terminal adapter.
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

// One decoded input action for UserSession, carrying the character when the action is a keystroke.
struct SessionInput {
    SessionInputKind kind{SessionInputKind::ignored};
    wchar_t character{};
};

// The seam between a session and the screen. It declares everything UserSession needs — read one
// input action, paint the conversation, editor, generation status and notice, scroll, resize — so
// session behavior can be exercised without curses. Tui is the curses implementation; tests
// substitute a recording one.
class SessionView {
public:
    virtual ~SessionView() = default;

    virtual std::optional<SessionInput> read_input() = 0;
    virtual void render(
        const Conversation& conversation,
        const InputEditor& editor,
        const GenerationStatus& status,
        bool show_addressing,
        std::string_view notice) = 0;
    virtual void scroll_up() = 0;
    virtual void scroll_down() = 0;
    virtual void resize() = 0;
};

} // namespace cha
