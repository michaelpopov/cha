#pragma once

namespace cha {

// Owns the process-wide terminal lifecycle, which only one component may. It sets the screen up on
// construction, switches input and cursor modes as the program moves between startup selection and
// chat, redraws after a resize, and restores the terminal on exit or destruction. StartupSelector
// and Tui borrow it rather than configuring the screen themselves.
class Terminal {
public:
    Terminal();
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    void configure_selector();
    void configure_chat();
    void resize();
    // Leaves ncurses mode; repeated calls are harmless.
    void restore();

private:
    bool active_{true};
};

} // namespace cha
