# Editing workspace entities

This note records the reusable lessons from adding persona creation, renaming,
and Markdown replacement to the web application. It is intended as a guide for
similar editable workspace features.

## Follow an existing interaction

When a new workflow resembles an existing one, copy its interaction shape. The
persona creation flow follows the session creation flow:

1. Put a small “New …” row at the top of the relevant list.
2. Open a focused screen containing one short form.
3. Trim and validate the entered name.
4. Create the entity, add it to application state, and navigate to its detail
   screen.

Keep entity-specific logic separate. Session creation also opens a live
conversation, while persona creation only creates configuration and opens its
detail screen.

## Persist through `WorkspaceConfigStore`

The SQLite configuration is authoritative at runtime. Do not edit the original
import directory or update the published `Workspace` object directly.

All configuration mutations should use `WorkspaceConfigStore::Impl::edit`:

1. Modify the materialized private workspace tree through a small `Workspace`
   write method.
2. Load the edited tree into a candidate `Workspace` to validate the complete
   result.
3. Collect its configuration rows.
4. Replace the SQLite rows in one transaction.
5. Publish the validated candidate only after the transaction commits.

This provides the same rollback and restart-required behavior for both edits
and newly created files. A creation method should build the minimum valid
directory, TOML file, and Markdown file inside that transaction. Validate names
and collisions before creating the directory whenever possible.

Use server-generated identifiers when the form asks only for a display name.
The identifier must satisfy the workspace's ID rules, avoid reserved IDs, and
be checked against the currently published workspace.

## Keep the API contract synchronized

For a new mutation, change all contract layers together:

- Add the route and request schema to `resources/cha.yaml`.
- Parse an exact JSON shape on the server and reject extra or incorrectly typed
  fields.
- Return the complete canonical entity after the mutation.
- Regenerate `webapp/src/api/schema.d.ts`.
- Add the typed client method and a request URL/body test.

Use `PATCH` for partial edits and `POST` on the collection for creation. Reuse
the common mutation checks, request-size limit, and error envelope.

## Update visible state immediately

After a successful mutation, do not wait for a later bootstrap refresh to make
the screen truthful. Update the bootstrap roster in the reducer and also patch
any visible references to the same entity, such as forum summaries or the
active session snapshot.

After creation, insert the returned summary into the roster and select the new
entity. The detail screen can then fetch the canonical Markdown normally.

Writability is a server fact. Load it with the detail response and hide editing
controls for built-in entities rather than attempting a mutation that is known
to be unsupported.

## File inputs in the browser and macOS app

The browser and packaged macOS app render the same HTML, but file selection has
one native integration requirement:

- Browsers handle `<input type="file">` themselves.
- A `WKWebView` whose owner installs a `WKUIDelegate` must implement
  `webView(_:runOpenPanelWith:initiatedByFrame:completionHandler:)`.

Without that delegate method, activating a file input in the packaged app can
do nothing—no error and no picker—even though browser and DOM tests pass. The
delegate should open an `NSOpenPanel`, copy the multiple-selection and
directory-selection flags from `WKOpenPanelParameters`, and call the completion
handler with selected URLs or `nil` on cancellation.

The web control can remain an icon button that activates a hidden file input.
Once a file is selected, both environments follow the same JavaScript upload
and database mutation path.

## Verification checklist

For this class of change, verify each boundary:

- Parser tests for valid, missing, extra, and incorrectly typed JSON fields.
- Route tests that inspect both the response and the stored SQLite rows.
- Bootstrap tests proving the new or renamed entity is published.
- UI tests for form validation, immediate roster updates, detail navigation,
  file reading, and mutation errors.
- Generated API type check, TypeScript check, and production web build.
- Native C++ build and affected web route suites.
- Direct Swift launcher compilation, or a full macOS package build, whenever a
  `WKUIDelegate` behavior changes.

DOM tests cannot prove that a native `WKWebView` panel appears. Treat the
packaged shell as a separate integration boundary and compile or exercise it
explicitly.
