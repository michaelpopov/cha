# Block 1: Configuration, selected-vault runtime, and credentials

## Task and starting point

Implement this block in the CHA repository. It is the first of four sequential
implementation blocks. This file is a self-contained brief; no earlier chat or
implementation-plan file is needed. The repository currently accepts one
external TOML file through `--config` and opens one workspace/session database.
Confirm that starting point before editing; preserve any work already done.

At completion, the runtime and console commands use a validated vault registry
and configuration directory. Normal server startup opens the configured vault.
OAuth and environment loading are application-wide. All affected code consumers
and test fixtures compile and work with the new configuration contract.

Runtime switching is block 2. Protocol/UI changes are block 3. Packaged
launchers and native first-run configuration are block 4; exercise this block
through the updated test harness or an explicit configuration directory.

## Working rules

- Read applicable `AGENTS.md`/`CLAUDE.md`. CHA is a personal application: use
  small, readable changes, existing helpers, and no speculative abstractions.
- Inspect `git status` and preserve unrelated changes. Do not commit databases,
  credentials, `.env`, build output, or other local runtime data.
- Implement only this block. Do not edit `docs/` or general user documentation
  during this block. Smaller commits are optional; this brief does not require
  committing.
- Paths below are repository-relative. Use `rg` to find additional callers and
  fixtures; the listed files are not an excuse to leave compile errors.
- The design reference is `docs/vaults-design.md`; the required behavior for
  this block is reproduced below. Retain the directory layout and explicit
  console vault selection. Do not substitute a single-file vault list.

## Required configuration behavior

`--config` now names an existing directory, not a TOML file. A file or missing
directory is an argument error. There is no compatibility mode. Example:

```text
cha-config/
├── app.toml
├── personal.toml
├── projects.toml
├── openai-auth.json
└── .env
```

`app.toml` contains only the required startup `vault`, `[web]`, and `[logging]`
settings. Retain existing validation for the two tables, including port 0:

```toml
vault = "Personal"

[web]
host = "127.0.0.1"
port = 8080

[logging]
file = "cha.log"
level = "info"
```

Every other direct-child `.toml` file is a vault definition. Do not recurse;
ignore files with other extensions. The filename has no identity. Example:

```toml
vault_name = "Personal"
data = "personal.sqlite3"
mirror = "personal-mirror"
modify = "personal-edit"
```

`vault_name` and `data` are required strings; `mirror` and `modify` are optional
paths. Reject unknown fields and wrong types. Reuse `validate_public_name` for
non-empty valid UTF-8 with no controls, line breaks, or surrounding whitespace.
Unicode, spaces, punctuation, and the name `Entrance` are allowed. Use the
existing `fold_ascii` for the planned case-insensitive matching and sorting;
non-ASCII bytes compare unchanged. Preserve authored spelling for display.

Resolve all relative paths, including `logging.file`, against the configuration
directory. Normalize paths to absolute, weakly canonical form using existing
helpers. Discovery does not open databases or require database, mirror, or
modify paths to exist: import must be able to initialize a missing database.
The registry is fixed until process restart.

Fail discovery for an unreadable, unparsable, or invalid vault definition, an
empty registry, duplicate folded names, or any of these path collisions:

- Equal database paths or equal database filenames. R2 continues to derive
  object identity from the percent-encoded database filename, unchanged.
- Two `modify` roots equal or containing one another.
- A `modify` root equal to or containing any vault's database or the
  configuration directory.

Reuse component-wise path containment; siblings are allowed. Do not add
hard-link detection or mirror-overlap validation. Diagnostics must identify
the source file/problem and, for collisions, both vaults/fields and the paths.

The required `app.toml` selection must name a discovered vault. Normal startup
fails if that vault's database cannot be loaded; there is no partial runtime.

## Console and native maintenance contract

```text
chaweb --config=CONFIG_DIR [--root PATH]
chaweb --config=CONFIG_DIR --vault=NAME --import SOURCE_DIRECTORY
chaweb --config=CONFIG_DIR --vault=NAME --export DESTINATION_DIRECTORY
chaweb --config=CONFIG_DIR --vault=NAME --upload
chaweb --config=CONFIG_DIR --vault=NAME --download
```

Accept `--vault=NAME` and `--vault NAME`. It is required for the four console
maintenance operations and rejected in normal server mode. Validate the whole
configuration registry before touching data. Match `--vault` using the same
name comparison; its selection applies only to that invocation and never
rewrites `app.toml`. Preserve the existing mutually exclusive operation rules.

Console import/export retain their explicit source/destination arguments.
Runtime/native import/export use the active vault's optional `modify` path;
they are unavailable without it. Preserve the import-source safety check,
updated to reject a source containing the configuration directory. Upload and
download use the selected vault's `data`, keeping existing R2 object names.

The native initial seed is a private pre-runtime operation. It loads the same
directory, resolves `app.toml`'s startup vault, and imports the bundled seed
only if that database is missing. It does not need a user-facing `--vault`
argument and does not rewrite the selection.

## Implementation

### Configuration types and parser

In `src/web/application_config.h` and `.cpp`, introduce these interfaces:

```cpp
struct VaultDefinition {
    std::string name;
    std::filesystem::path data;
    std::optional<std::filesystem::path> mirror;
    std::optional<std::filesystem::path> modify;
};

struct ConfigurationDirectory {
    std::filesystem::path directory;
    std::string startup_vault;
    std::vector<VaultDefinition> vaults;
    std::string host;
    int port{};
    std::filesystem::path log_file;
    std::string log_level;
};

ConfigurationDirectory load_configuration_directory(
    const std::filesystem::path& directory);
bool same_vault_name(std::string_view left, std::string_view right);
const VaultDefinition* find_vault(
    const std::vector<VaultDefinition>& vaults, std::string_view name);
```

`find_vault` returns null for an unknown name. Keep the vector sorted by folded
name. Reuse `required_string`, `reject_unknown_fields`, `resolve_config_path`,
`normalize_cli_path`, and `path_is_under` where suitable. Name utilities already
live in `src/util/text.h` (`fold_ascii`) and `src/util/public_name.h`
(`validate_public_name`). R2 identity code in `src/web/r2_database_transfer.cpp`
is unchanged; enforce filename uniqueness in configuration validation instead.

Replace `ApplicationCommand.database`, `.mirror`, and `.modify` with
`config_directory`, `std::vector<VaultDefinition> vaults`, and
`VaultDefinition vault`. Keep the existing operation flags, import/export
arguments, assets root, web/logging settings, and test idle-grace option.
`command.vault` is the resolved startup/console selection, not mutable runtime
state. Update `web_usage` to the command forms above.

### Selected-vault runtime and credentials

In `src/web/current_vault.h`, provide the small ownership helper planned for
the runtime: `CurrentVault(VaultDefinition)`, `VaultDefinition get() const`,
and `void set(VaultDefinition)`, with a mutex protecting the stored value.
`ApplicationRuntime::Impl` owns one initialized from `command.vault`.
Expose `VaultDefinition ApplicationRuntime::current_vault() const` as a copy.
This mutex protects the value only; it is not an atomic switch of all runtime
resources. Block 2 completes maintenance synchronization.

Open `WorkspaceConfigStore` using the selected vault's data; construct the
existing repository and optional mirror from its paths. Change runtime upload,
download, import, and export to read one current-vault copy **inside** the
callback already serialized by `maintain_database`'s lifecycle mutex. Read
`data` and `modify` from the same copy, and check the optional `modify` there.
Do not first convert these methods to startup-only `command.vault` accesses
that the next block would have to replace.

Load `command.config_directory / ".env"` once at runtime startup, before
opening the store or constructing providers. An uncomplicated location is
`ApplicationRuntime::open`, before constructing `Impl`. The store currently
opens in the `Impl` initializer list: loading `.env` in its constructor body
would be too late. Remove the database-adjacent `load_dotenv` call from
`WorkspaceConfigStore::open`. Missing `.env` is allowed; inherited variables
win. Console maintenance keeps using the process environment as before.

Construct one `OpenAiOAuth` with `config_directory / "openai-auth.json"`.
Keep existing private-file validation and permissions. `Providers` and its
factory continue using that stable owner. Credentials and `.env` are outside
workspace export, mirrors, and R2 transfer. Do not migrate real user files or
add a fallback to the old database-adjacent credential path.

### Consumers and fixtures: update in this block

- `src/web_main.cpp`: resolve all four console operations and their diagnostic
  paths through `command.vault.data`; retain explicit import/export arguments.
- `packaging/macos/runtime_bridge.cpp` and `.h`: use the directory loader for
  initial seeding; check the selected-vault lookup before dereferencing it.
  Remove cached `ChaRuntime::can_modify` and have `cha_runtime_can_modify`
  query `application->current_vault().modify`. Preserve null/error handling
  and the C boundary's rule that exceptions never escape to Swift. Update
  comments describing its configuration argument. Swift setup is block 4.
- `tests/web/unit_application_runtime.cpp`: update `make_command` and **all**
  direct uses of the removed command fields now. Create a real temporary
  configuration directory with complete `app.toml` and vault files, and fill
  the registry and selection consistently. Update OAuth fixture helpers to
  write in that directory. Do not leave this fixture migration until switching.
- `tests/web/unit_application_config.cpp`: use directory fixtures with
  `write_app` and `write_vault` helpers and complete web/logging tables.
- `tests/support/web_server_process.cpp` and
  `tests/web/process_web_server.cpp`: generate `app.toml` plus a `Test` vault
  file, pass the directory to the server, and pass `--vault=Test` to offline
  commands. Preserve isolation and cleanup of generated directories.
- `webapp/e2e/start-cha.mjs`: generate a complete `cha-config/app.toml` and
  `e2e.toml` naming `E2E`, and use `--vault=E2E` for import. The packaged-file
  assertion still checks the old example filename until block 4 changes the
  package producers and consumers together.
- `src/workspace/workspace_config_store.cpp` and
  `src/session/workspace_session_database.cpp`: change import hints to
  `chaweb --config=CONFIG_DIR --vault=NAME --import WORKSPACE`, updating the
  process-test assertions at the same time.

Use `rg` for all remaining `command.database`, `command.mirror`,
`command.modify`, designated initializers, and generated old config files.
Do not change references to a legacy root `app.toml` **inside an import source**;
that is unrelated to the new application configuration directory.

## Verification and acceptance

Before editing, establish the baseline with `make test` and, from `webapp`,
`npm ci` followed by `npm run check`. Record existing failures without taking
on unrelated fixes. Use existing dependency installations for later checks.

Extend configuration tests to cover valid multiple vaults, relative paths,
canonical spelling and ordering, Unicode name validation, file-as-config
rejection, missing/invalid app or vault files, empty registry, unknown fields,
duplicate names/data paths/filenames, nested modify collisions, an unknown
startup selection, and explicit console selection/argument errors. Exercise
symlinked parents where supported, using the existing path normalization.

Runtime/process tests must prove:

- Server startup opens the configured vault; a missing startup database fails.
- Console import can create the explicitly selected missing database, and
  invalid/missing vault selections fail before data changes.
- Existing import/export/upload/download behavior remains valid with the new
  command shape; nonselected database contents are untouched.
- Configuration-directory `.env` loads before workspace/provider use and
  never overrides an inherited variable; database-adjacent `.env` is ignored.
- OAuth routes load/write the configuration-directory file; existing login
  validation and private-file checks still pass.
- Updated C++ fixtures, including runtime fixtures, compile in this block.

After implementation run, from the repository root:

```bash
make test
(cd webapp && npm run check)
make web-e2e
```

The browser E2E command builds the browser bundle and uses `build/ninja/chaweb`
from `make test`; ensure Playwright's browser is available. Report platform or
dependency limitations precisely. Do not weaken tests to make a check pass.

Finish with a handoff listing the new interfaces, configuration/credential
behavior, and command results. Block 2 must find `VaultDefinition`, directory
loading, a populated `ApplicationCommand` registry, `CurrentVault`, the runtime
getter, and maintenance methods already reading the active selection under
the lifecycle mutex. No switch API or browser vault fields are required yet.
