#pragma once

#include "generation_status.h"

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

// Carries one typed terminal action and its character payload when applicable.
struct SessionInput {
    SessionInputKind kind{SessionInputKind::ignored};
    wchar_t character{};
};

// Abstracts terminal input and rendering so session behavior can be tested without curses.
class SessionView {
public:
    virtual ~SessionView() = default;

    [[nodiscard]] virtual std::optional<SessionInput> read_input() = 0;
    virtual void render(
        const Conversation& conversation,
        const InputEditor& editor,
        const GenerationStatus& status,
        bool show_addressing,
        std::string_view notice) = 0;
    virtual void scroll_up() = 0;
    virtual void scroll_down() = 0;
    virtual void resize() = 0;
    [[nodiscard]] virtual bool input_closed() const = 0;
};

} // namespace cha
