# Utility sources

`util/` contains small, broadly reusable helpers that do not belong to a
domain-specific component. Utilities here should remain focused, deterministic
where practical, and independent of the higher-level Cha modules.

## Contents

| Source | Responsibility |
| --- | --- |
| `text.*` | Byte-oriented whitespace detection, trimming, and ASCII case folding. |
| `path_name.*` | Validation that a configured name is one safe filesystem path component. |
| `environment.*` | Optional `.env` loading without replacing variables already present in the process environment. |

## Functionality

The text helpers centralize the ASCII and byte-string rules used by command
parsing, agent-handle matching, list-file parsing, and environment loading.
They deliberately do not attempt locale-sensitive Unicode normalization.

`require_path_component()` rejects empty names, absolute paths, parent paths,
`.` and `..`. Storage code uses it before turning workspace-controlled names
into filesystem paths.

`load_dotenv()` accepts blank lines, comments, and simple quoted or unquoted
assignments. A missing file is harmless; malformed entries and unreadable
existing files are errors. Existing environment variables always win.

## Dependencies

This is a leaf module:

- it depends only on the C++ standard library and the process environment;
- it must not depend on `conversation/`, `agents/`, `storage/`,
  `application/`, or `interfaces/`;
- callers currently include agent identity and roster code, storage loaders,
  text interfaces, and the TUI composition root.

If a helper begins to encode conversation, agent, persistence, or interface
policy, it belongs in that owning directory rather than here.
