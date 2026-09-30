# Block 4: ChaWeb conversation behavior

Status: implemented and checked in the iOS Simulator. The real iPhone checks
in "Results" at the end are outstanding.
This block implements plan step 8 and its browser tests from step 9. It requires
the daemon API, nginx path, HTTP client, and two browser views from blocks 1–3.
The real iPhone layout, keyboard, size-toggle, and dictation checks must have
passed before starting this work.

This document contains the requirements for this block. Background:
[chaweb.md](chaweb.md) and [chaweb-plan.md](chaweb-plan.md).

## Result, files, and boundaries

Deliver working session creation, continuation, Send, Stop, snapshot polling,
draft preservation, and recovery. Keep the existing full-screen UI and stable
textarea. Backend state remains authoritative; the browser owns navigation,
unsent drafts, and request state.

Work under `webapp/src/chaweb/`: connect `App.tsx`, `client.ts`, and the components
from block 3, adding small state modules/hooks and adjacent Vitest tests as
needed. `webapp/src/useLiveSession.ts` is a reference for navigation and draft
handling, not a transport to import wholesale. Reuse shared types, guards,
Markdown, and character appearance code. Remove the temporary transcript-update
fixture used for the early iPhone check.

No new runtime/DTO changes, endpoints, title polling, persistent drafts,
background outbox, multi-tab coordination, or synchronization framework. Keep
configuration editing, recipient selection, renaming, deletion, application
voice, and offline operation out of scope. Text such as `@Name`, `/mcast`, and
`@-` goes unchanged to CHA; do not implement a browser command parser.

## API and transport assumptions

All paths are relative to `/api/cha/v1`. `S` means
`/forums/{forum_id}/sessions/{session_id}`. Use IDs, never titles, for identity.

| Request | Body | Success |
| --- | --- | --- |
| `GET /bootstrap` | None | `200`, existing `Bootstrap`. |
| `GET /forums/{forum_id}/sessions` | None | `200`, `SessionListing[]`. |
| `POST /forums/{forum_id}/sessions` | `{ "text": "..." }` | `201`, `{ "id": "...", "label": "..." }`. |
| `GET S` | None | `200`, `SessionSnapshot`. |
| `POST S/input` | `{ "text": "..." }` | `204`, no body. |
| `POST S/stop` | `{}` | `204`, no body. |

Block 3's client sets `application/json` on all POSTs, uses default same-origin
credentials, rejects redirects, validates existing DTOs, and applies a 60-second
request timeout. Preserve these choices. Use `validateBootstrap()`,
`isSessionListingArray()`, `isSessionLabelResult()`, and `isSessionSnapshot()`.
Keep shared guards tolerant of extra response fields. `204` is successful
without JSON parsing; fetch the notice/state in a snapshot afterward.

Errors contain `{"error":{"code":"...","message":"..."}}`. A `404`
refreshes navigation lists, `422 invalid_argument` shows CHA's safe notice, and
other failures show a safe message. Preserve drafts in every case. A non-JSON
nginx error uses a generic message. The adapter uses `400`, `404`, `415`, `422`,
`500`, and `503`; transport/proxy failures can use other statuses.

Input acknowledgement means acceptance, not completed generation. Jev/naming
can delay acceptance by about ten seconds under current settings. The daemon
handles HTTP requests serially, so Stop and reads can wait behind input. Existing
runtime workers continue accepted turns after an HTTP response or disconnect.
A timed-out command returned as `500` can still execute; no mutation retry is
safe merely because an error or an empty recovery snapshot was received.

## State and navigation

Keep only the state needed for:

- Selected view/forum/session and the validated bootstrap/session list.
- One local new-conversation draft per forum, plus stored-session drafts keyed
  by forum and session. Track edits so acknowledgements cannot erase newer text.
- Current authoritative snapshot, editor size mode, and whether the transcript
  follows the end.
- Pending commands, unknown write outcomes, read scheduling/retries, and a
  navigation generation or equivalent local marker for rejecting stale results.

Do not maintain another durable transcript, derive acknowledgements from text
matching, or put drafts in localStorage. Drafts and editor mode survive in-app
navigation and network recovery, but not page reloads.

Stored sessions use `/#/forums/{forum_id}/sessions/{session_id}`. On startup,
load/validate complete bootstrap before hiding Entrance using
`entrance_forum_id`; ignore Welcome/initial-session fields for automatic opening.
Open a valid stored route or show the forum/session list. Maintain Back/Forward
support without duplicate requests from programmatic route changes.

The list and new conversation are local views. New Session clears the stored
hash and returns to the selected forum's existing local draft, or creates an
empty local draft. It makes no create, poll, or cleanup request. The Sessions
icon shows that conversation's forum and dismisses the keyboard while keeping
the draft. Opening a row, including the current session, fetches its state
without autofocus.

Every asynchronous result retains its originating forum/session or local draft
and navigation marker. Ignore stale read results after selection changes. A
write result may update its originating draft but must not replace a different
view's route, snapshot, error, or text. Keep track of pending operations when
the user leaves and returns to their originating conversation.

## Sending and stopping

For a local new conversation:

1. Enable Send when bootstrap supplies a valid forum and the draft can be sent.
   Do not wait for a nonexistent session snapshot. Leave the draft editable.
2. On Send, record its text/revision and call creation with exactly that text.
   Prevent duplicate creation while the request is pending. Stop remains
   disabled because no session ID is known.
3. On `201`, associate the originating draft with the returned session ID and
   clear only the submitted revision. Preserve edits made while waiting.
   Set the stored-session hash only if that draft is still the current view.
4. Refresh the forum's session list. If the conversation is still viewed and
   visible, fetch its first snapshot. Stop can now address the returned ID.
5. A definitive `422` keeps the draft; successful server cleanup leaves no
   stored session. Other uncertain results follow the recovery rules below.

For an existing session, wait for a valid initial snapshot before Send. Disable
Send while creation/input/Stop is pending, generation is active, or session
state/write outcome is unknown. The textarea stays editable. Do not enforce a
client-side prompt-length limit; the server checks UTF-8 bytes and failures keep
the draft.

On later Send, record the submitted draft revision and call `POST S/input`.
On `204`, clear only that revision, request a snapshot, and refresh the session
list. Keep the accepted input visibly pending until authoritative state arrives;
do not insert a fabricated durable transcript entry or infer exact acceptance
from matching text. Self-notes may complete without any character reply.

For a known session, allow Stop while input is pending or generation is active,
and as an explicit action when current state is unknown. Dispatch it immediately;
do not wait in the browser for the input response. The serial daemon may still
process it after classification/naming. Show Stop requested while pending, then
keep stopping state until a subsequent snapshot shows generation inactive.
A successful `204` alone does not establish that final state. Idle Stop is safe
and may be repeated; avoid stacking duplicate in-flight Stop requests.

Keep Stop disabled throughout first Send until `201`. Do not queue a pending
Stop for an unknown session, invent an ID, or automatically stop a late-created
session because the user navigated away. Leaving a conversation or closing its
request does not cancel its generation.

## Snapshot scheduling and rendering

`GET S` opens/selects the stored session and returns a full owning snapshot.
Check `snapshot.forum.id` and `snapshot.session_id` against the requested
identity before applying it. Keep notice, title, generation, and transcript from
the snapshot. Extra reasoning/audio/cleanup fields do not activate UI features.

- At most one snapshot read may be outstanding for the browser view, including
  across navigation. Coalesce triggers while a read is running; discard its
  result if stale, then service the current selection's pending refresh.
- Fetch on selection, successful creation/input/Stop acknowledgement, and return
  to the foreground when a stored conversation is visible. A local new draft
  has no snapshot to fetch.
- Schedule each routine read one second after the previous read finishes. Use
  completion-driven timers, not a repeating interval that accumulates requests.
- Continue routine polling only while the conversation and page are visible
  and the latest snapshot has `generation.active === true`.
- On navigation or hiding, stop scheduling the old view's reads. An already
  running read can finish; its result must pass the current identity checks.
  Fetch current state when returning. Backend generation continues meanwhile.
- Commands do not suspend polling. Do not abort a running read to get Stop
  ahead of it; send the command and let the daemon's serial loop handle it.

If an acknowledgement arrives during a read, queue a fresh read after it.
A snapshot fetched before the acknowledged command is not proof of that
command's resulting state, even when it belongs to the current session.

Replace snapshot state rather than appending/concatenating received text. Key
entries by their stable IDs. Keep the textarea mounted and its value tied only
to its draft. Preserve focus, selection, editor size mode, and reading position
across snapshots, notices, and list refreshes. Follow output only when the user
is near the bottom; scrolling up suspends following until they return near it.

Refresh session lists on navigation, creation, accepted input, observed title
change, generation completion, and return to the foreground. Show recent
sessions using their `updated_at`; `live` is not a generation flag. Do not poll
for naming alone or add `naming_active`. After a short reply or self-note, a
temporary title can remain until an ordinary later refresh.

## Failure and recovery

For retryable read failures, keep the last transcript and draft, show
Reconnecting, and retry after 1, 2, 4, then at most 10 seconds. Retry only for
the still-relevant visible view. On recovery, refresh bootstrap and then the
selected snapshot; retain the same stored IDs across a daemon restart.
Coalesce recovery with ordinary refreshes so it cannot create concurrent polls.

A session `404` stops its polling and refreshes forum/session choices. Keep its
draft associated with the original identity. Malformed response data is an
actionable error, not an infinite retry loop. Persistent read failures must
also end with an error and an explicit retry path; use a small bounded retry
policy rather than adding settings or background recovery machinery. While
current session state is unknown, disable Send but permit explicit Stop for a
known session. Apply these read rules to startup/list failures as appropriate.

Never automatically replay creation or input after a timeout, connection loss,
or another failure with uncertain side effects. Show:

> Send status unknown. Check the conversation before sending again.

For a known session, preserve the draft and fetch current snapshots. For first
Send without a returned ID, preserve the local draft and refresh the forum's
list so the user can select and inspect any created session. An absent row or
entry on the first refresh does not prove failure; neither does a `500` command
timeout. Do not resolve an uncertain acknowledgement by matching transcript
text. Require a deliberate user decision after inspection before another Send;
recovery must not silently become Resend or erase the draft.

Definitive validation/rejection errors can leave Send usable after the error is
shown and normal readiness is restored. Distinguish these from transport and
timeout uncertainty using the available response, not the HTTP status alone.
There is no exactly-once guarantee, idempotency key, request ledger, or outbox
in stage 1. An explicit Stop may be repeated because its effect is safe.

## Automated tests

Use Vitest, fake fetch, controlled promises, and fake timers under
`webapp/src/chaweb/`. Existing discovery includes these tests. Reuse complete
DTO fixtures and the production state logic; do not introduce another backend
or a Playwright suite.

| Area | Required cases |
| --- | --- |
| Draft lifecycle | New Session makes no mutation; one new draft per forum; stored drafts survive navigation; accepted acknowledgements clear only submitted revisions; failures preserve text. |
| Navigation | Back/Forward; stale snapshots/lists ignored; response identity checked; late creation associates its originating draft without changing another view; returning to a pending command preserves its state. |
| Send and Stop | First Send waits for `201` before Stop; later `204` is not parsed; self-notes finish; known-session Stop dispatches during pending input and waits for authoritative idle state. |
| Polling | One outstanding read, including navigation; one-second delay after completion; coalesced triggers and a fresh read after an acknowledgement; no accumulation on slow requests; hidden/list/local-draft views do not poll; commands do not suppress reads. |
| Titles | Generation completion/title changes refresh lists; inactive generation stops polling even when naming continues. |
| Recovery | Backoff and foreground refresh; terminal malformed data and `404`; lost first/later acknowledgements and `500 command_timeout` never replay input; early absent data does not prove failure. |
| Editor | Snapshot updates retain the textarea node, draft, caret, size mode, and reading position; IME/dictation text changes never submit by themselves. |

Run focused frontend tests while developing. Before handing off to packaging:

```sh
make test
make web-check
make itest-local
make itest-daemon
```

Use local/fake providers for routine verification. Existing native/OpenAI tests
remain part of regression coverage; no new multi-user fixtures are needed.

## Manual check and completion

Use the local daemon and nginx listener through Vite's proxy. Run
`npm --prefix webapp run dev:chaweb -- --host 0.0.0.0` and open its LAN HTTP URL
on the iPhone. The local setup supplies existing configured forums/provider
settings; there is no browser configuration screen.

Create a conversation with first Send, continue an older one, send ordinary
text and CHA addressing commands, and Stop a long reply. Navigate away during
generation and return. Edit the draft while requests are pending. Try a
rejected first input, an oversized prompt, and a self-note. Check that a
rejected creation leaves no row and that self-notes remain stored.

Simulate failed reads and lost Send responses, inspect the list/transcript,
and verify no duplicate turn is sent automatically. Restart the daemon on the
same vault and recover the selected session. Background Safari and return.

Repeat keyboard dictation while real snapshots arrive, including expanded
editor mode, rotation, and reading older messages. Verify the editor, caret,
keyboard, and reading position remain stable. This uses the actual polling
logic; the earlier layout fixture is no longer sufficient.

Completion requires passing automated checks and the real chat/recovery/iPhone
checks above. Record results and outstanding environmental limitations plainly.
The handoff is a working browser-to-nginx-to-daemon text application ready to
package, with stable IDs and additive response handling left available for
future voice work.

## Results

Automated checks pass: `make test` (960 tests; the two live OpenAI tests are
skipped without credentials), `make web-check` (902 tests), `make itest-local`
and `make itest-daemon`. The temporary transcript-update fixture is removed.

Manual check in the iOS Simulator (iPhone 17, iOS 27 Safari) through Vite, a
local nginx listener and the daemon on a test vault. A fake Chat Completions
provider answered after 3 seconds, or after 15 seconds for a long reply.
These cases behaved as specified, verified in the nginx access log:

- New Session makes no request. First Send gets `201`, then one list refresh
  and one snapshot; Stop stays disabled until the ID is known.
- Later Send gets `204`; the draft clears only after acceptance. Polling runs
  one read at a time, one second after the previous read, and stops when
  generation ends; the list then refreshes.
- Stop during a long reply gets `204`; reads continue until a snapshot shows
  generation inactive.
- Leaving during generation stops reads; returning fetches the finished reply
  and keeps the draft typed while pending. Back and Forward make one request
  each. Opening a session does not open the keyboard.
- Rejected first input (`/not-a-command`) shows CHA's notice, keeps the draft
  and leaves no session row. A first self-note is stored and finishes without
  a reply. An oversized prompt shows "Prompt is too large." and keeps the draft.
- Daemon restart: Reconnecting, retries after 1, 2, 4 and 10 seconds, then
  bootstrap and snapshot with the same session ID.
- Safari in the background: no reads while hidden; one refresh on return.
- Lost input response (daemon paused past the 60-second timeout): "Send status
  unknown" shows, the draft stays, no replay. Send is available again only
  after a fresh snapshot.
- Expanded editor: the mode survives Send; typing, caret, open keyboard and a
  scrolled-up reading position survive arriving snapshots.

Decisions: Send closes the keyboard, and sending while scrolled up does not
jump to the end. Both are kept as they are.

Outstanding, because they need a real iPhone: the LAN check with
`dev:chaweb -- --host 0.0.0.0`, keyboard dictation with the real microphone
and IME composition, rotation, and real network loss. The simulator types text
directly and cannot rotate from the test tools.
