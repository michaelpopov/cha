# Shared transcript rendering

`ui/render/` defines the presentation vocabulary shared by the TUI and console
frontends. It knows how transcript entries are labelled and styled, but it
does not know about curses, screen dimensions, descriptors, or output streams.

`TranscriptSurface` is the output seam. A frontend supplies a surface that can
select `normal`, `bold`, `dim`, or `bold_dim` attributes and write text.
`write_transcript_entry()`, `write_active_response()`, and
`write_transcript_suffix()` then produce the same labels and styling in either
frontend. `show_addressing()` enables `[You → Name]` labels for multi-agent
forums and for restored history involving agents no longer in the forum.

This directory deliberately excludes layout and redraw policy. Curses-specific
incremental planning, viewport state, wrapping, and `wcwidth` calculations live
in `ui/tui/`. Append-only stream tracking lives in `ui/console/`.

## Dependencies

- **May depend on:** `session/` presentation values and `transcript/`.
- **Must not depend on:** either frontend, curses, terminal geometry, or
  process I/O.

The focused tests are in `tests/ui/render/`.
