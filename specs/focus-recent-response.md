# Focus the recent prompt and response

## Purpose and scope

Bring the latest submitted prompt into view. When automatic voice generation
is enabled, bring a model response into view when its first readable text
arrives.

This is a technical design, not an implementation. It covers desktop Chat on
macOS and Windows and the separate ChaWeb conversation screen. It adds no
controls, settings, labels, visual highlights, or persisted data. Notes and its
prompt are outside this feature.

“Focus” means moving the transcript viewport. It does not mean DOM focus:
the composer must keep its keyboard focus, selection, draft, and microphone
state.

## Behavior

| Event | Required scroll behavior |
| --- | --- |
| A submitted prompt appears in the transcript | Bring that prompt into view, even if the reader has scrolled up. Apply this to typed and voice prompts, with automatic audio on or off. |
| The first readable text of a response arrives while automatic audio is on | Bring that response into view once, even if the reader has scrolled up. |
| The first readable text arrives while automatic audio is off | Keep the current behavior: follow the end only when the reader was already following it. |
| More response text streams, or the response completes | Keep the current follow behavior. Do not force another scroll for the same response. |
| The reader scrolls away | Keep their position until another qualifying prompt or newly readable response arrives, or they scroll back to the end. |
| Automatic audio is enabled or disabled | Do not scroll merely because the setting changed. |
| Audio is queued, generated, downloaded, played, stopped, retried, or cleared | Do not scroll merely because audio state changed. |

Readable text means non-whitespace text in the rendered message body of a
character entry whose status is `streaming` or `complete`. Inspect only
`.cha-message-text` or `.chaweb-entry-text`, excluding the speaker, timestamp,
status, and controls. Empty placeholders, whitespace, and a Markdown body with
no text do not qualify. Desktop's legacy timestamp removal applies before this
check. There is no minimum length or sentence boundary.

The first readable text can arrive in an existing empty streaming entry or in
an entry first observed with content. A complete response first observed with
readable text also qualifies. Failed and cancelled entries do not qualify.
This timing supports desktop WebSocket audio that starts during generation.

Use the existing automatic audio flag: `autoAudioEnabled` in desktop Chat and
`speech.automatic` in ChaWeb. Do not infer this mode from microphone activity,
provider availability, cached audio, or a selected speaker button. Evaluate the
flag when the first readable text is observed. Record that observation even
when the mode is off. Enabling audio after text has already appeared does not
retroactively scroll to that response; enabling it before the first readable
text allows that arrival to trigger the scroll.

Scroll when accepted content is rendered, not when Send is clicked, voice
transcription changes, or a command promise resolves. A failed send with no
new transcript prompt does not cause a forced scroll. This also avoids a race
between a command acknowledgement and its transcript update. Self-notes use
the same prompt rule when their human entry appears.

Do not wait for speech synthesis or playback. The text response must become
visible even if its audio is delayed or fails. With multiple characters, each
response can become the target when it first has readable text. If one commit
contains both a locally submitted prompt and response text, the prompt takes
priority for that commit, including the first snapshot of a newly created
session. Otherwise select the last qualifying entry in transcript order.
Record all observed arrivals even when only one target is selected; do not
schedule a delayed jump to another candidate. Audio readiness does not affect
this decision.

## Viewport placement

Use an immediate scroll within the transcript container. Align the target's
top with the transcript's visible top, subject to the container's normal scroll
limits. A short final message will sit lower when there is insufficient content
below it to reach the top. Do not add blank space to force exact alignment.

For a message taller than the viewport, show its beginning. The reader can
scroll through the rest. Do not scroll to the end of a long response and hide
its opening text. No smooth animation is needed.

The following sketches describe scroll positions, not new screen elements:

```text
Reader looking at older content       New prompt rendered
┌─ Transcript ─────────────────┐       ┌─ Transcript ─────────────────┐
│ Earlier conversation        │  →    │ New prompt                  │
│ Earlier response            │       │                             │
└─────────────────────────────┘       └─────────────────────────────┘
  [Existing composer]                   [Existing composer]

Automatic audio on; first readable response text
┌─ Transcript ─────────────────┐
│ Latest response: beginning   │
│ More response text…          │
└─────────────────────────────┘
  [Existing composer]
```

Move only `.cha-transcript` or `.chaweb-transcript`. The window, sidebar,
picture panel, Notes pane, and composer must not move. Use container-relative
geometry rather than an unrestricted `scrollIntoView`, which can also move
ancestor scroll containers. This rule applies to forced placement, initial
history placement, normal end-follow, and resize callbacks.

After placement, update the existing following flag from the resulting scroll
position using the existing 24-pixel threshold. Do not set it to true
unconditionally: the beginning of a long response can be far from the bottom.
Manual scrolling continues to update this flag in the usual way.

## Existing implementation

- `webapp/src/components/ChatScreen.tsx` owns the desktop transcript,
  `transcriptArea`, `transcriptEnd`, `followingLatest`, and automatic audio.
  `followTranscript` currently scrolls to the end on transcript or generation
  changes when following is enabled. A `ResizeObserver` also follows container
  height changes.
- The same file renders each message as a `TranscriptMessage` article.
  `visibleTranscriptEntries` hides empty markers and repeated human entries
  used by multicast. Covered and uncovered messages have separate render loops.
- `PromptInput.tsx` routes typed and voice submissions through the same
  `onSubmit` prop. `App.tsx` performs the mutation, while the session stream
  supplies the transcript. The shared submission path can mark one pending
  prompt without separate keyboard or microphone handlers.
- `webapp/src/chaweb/transcript.tsx` has its own renderer, repeated-prompt
  filter, `following` ref, and layout effect that scrolls to the end. It receives
  `speech` through `conversation.tsx`, so its automatic flag is already present.
  ChaWeb currently passes an empty array before a snapshot is available.
- `resources/dto.yaml` already supplies entry identity, kind, text, and status.
  `src/chat/transcript.cpp` appends text to a streaming entry without changing
  its identity. Readable text can therefore arrive without changing entry count.

## Technical changes

### Detect transcript transitions

In each transcript owner, retain one ref containing the context key and an
ID-indexed observation for each entry in the last committed transcript. Each
observation needs only whether the response has ever had readable text. The
map's keys also identify previously present human entries. Carry the readable
bit forward for an existing ID, and drop IDs removed from the transcript.
This prevents a Markdown render that briefly loses its visible text from
causing a second first-text event. Do not also retain a transcript copy or a
second seen-ID set.

The context key must include vault, forum, and session identity. Desktop can
use the existing `audioConversationKey` value, which includes all three; do
not couple the scrolling baseline to audio cache clear counts. ChaWeb should
receive a context key composed from the vault name and actual selected forum
and session identity through `Conversation`. Use the draft identity before
session creation. Do not use `audioKey` as session identity: `createdOk` can
retain the draft audio key when it moves to the created session. Retain the
existing audio key and its audio behavior.

Compare committed transcripts after React has rendered their articles:

1. On the first loaded historical snapshot for a context, record the baseline
   and retain the initial scroll to the end. Existing readable text is already
   observed. An empty streaming response can still qualify when text arrives.
   A pending local submission is an exception, described below.
2. A newly present human entry that has its own rendered article is a prompt
   candidate. Compare raw entry IDs so deleting a message and exposing a
   previously hidden copy does not make that copy a new arrival.
3. A readable character entry with no previous readable observation is a
   response candidate if its status is streaming or complete and automatic
   audio is currently enabled.
4. Resolve any pending local prompt first. If it has arrived, target its
   article or visible representative. Otherwise use the last candidate in
   transcript order. Perform at most one forced placement per commit.
5. Record all current entries and readable observations, including while
   automatic audio is off or another target wins. Completion, further tokens,
   and mode toggles cannot retrigger a response already observed as readable.

Use stable entry IDs, not array length, text equality, timestamps, or audio job
state, to detect arrivals. Message text is used only by the existing display
filter to locate the visible representative of a suppressed human entry.

### Identify local prompts, including identical submissions

The current display filter suppresses both multicast copies and separate human
submissions with identical text and participant. Keep this display policy,
but let a new local submission target the existing visible representative.

Retain one pending prompt marker for the active view. It contains the context,
the greatest raw entry ID present when Send starts (zero for an empty/new
session), and an attempt identity for guarding asynchronous callbacks. Entry
IDs increase within a session. The marker is needed because the transcript
alone does not distinguish a separately submitted identical prompt from an
internal multicast copy. It does not store another copy of prompt text.

- Set the marker immediately before invoking the valid shared typed/voice
  submission path. Setting it does not scroll or enable end-follow.
- The first human entry above the saved ID boundary is the accepted prompt.
  If hidden, resolve it to the article retained by the existing participant
  and text comparison. The display helper can return this representative ID
  during its existing pass; no persistent DOM or duplicate-message index is
  needed. Use the new raw entry's identity to consume the marker once.
- A later hidden multicast copy has no pending local marker, so it does not
  force another jump. Sending the same prompt again creates a new marker and
  targets its representative again, including with automatic audio off.
- Consume the marker when its prompt is rendered, regardless of whether the
  command acknowledgement has arrived. A late success must not recreate it.
  A success before the snapshot leaves it pending until the prompt appears.
- Clear an unconsumed marker on a rejected/unconsumed submission, navigation
  away, vault change, or unmount. Guard callbacks by attempt identity so an
  older result cannot clear a newer marker. An uncertain transport outcome
  can retain the marker through the existing reconciliation read; that read
  either supplies the prompt or settles the marker without a forced scroll.
  Add no expiry timer, retry loop, or per-session queue.

Desktop should wrap `onSubmitInput` in `ChatScreen`, which already receives
typed and voice submissions from `PromptInput`. Hold the marker in a ref and
clear the matching attempt on rejection or `clear_input === false`. Match
against the committed transcript in the layout effect; do not scroll from
the promise handler.

ChaWeb should own the marker in `useChaweb` beside its existing send state and
set it in `send`, where both input methods converge. Pass it and a guarded
consume callback through `App` and `Conversation` to `Transcript`. Extend the
existing success/failure and reconciliation paths to settle this marker.
Use state here so transferring the marker to a created session is rendered.
The transcript observation ref and this one pending marker are the only new
scroll-related state; both have a specific role.

### Handle first snapshots and reconnects

Add a loaded-snapshot boolean to the ChaWeb transcript props, supplied from
`model.snapshot !== null`. Do not treat its temporary empty-array placeholder
as a loaded baseline. A loaded empty conversation is a valid baseline, so its
first subsequent prompt is detected normally.

When ChaWeb Send creates a session, transfer the same pending marker from the
draft context to the returned session in `createdOk`, before loading its first
snapshot, and only if that submission's draft is still the active view. Its
boundary remains zero. On the first loaded snapshot, resolve and focus the
accepted prompt before applying the historical baseline rule. This priority
holds even when that snapshot already contains a long response; do not replace
the prompt placement with an initial end-scroll. Record all entries in that
snapshot as observed afterward. Opening an existing session has no such marker
and continues to start at the end. A creation failure or navigation away must
not leave a marker that a later history open could consume.

Keep the baseline through a temporary connection loss in the same context.
When a replacement snapshot arrives, compare it with the last loaded snapshot:
new prompts and responses that became readable during the gap qualify. Identical replayed
snapshots do not qualify. Reset on an actual vault/forum/session change or
component unmount. No scroll-position persistence is needed.

### Find and place the target

Add a nonvisual `data-entry-id` attribute to each message article. On desktop,
add it inside `TranscriptMessage` so both covered and uncovered render paths
are covered. Add it to the ChaWeb articles as well. Query only inside the
current transcript container; no document-wide selectors or DOM node registry
are necessary.

Compute the desired scroll offset from the target and container rectangles:

```text
desiredTop = container.scrollTop
           + targetRect.top - containerRect.top - container.clientTop
container.scrollTop = clamp(desiredTop, 0,
                            container.scrollHeight - container.clientHeight)
```

Clamp the maximum to at least zero. Read the resulting offset to update the
following flag. No target node means no forced scroll; use the normal follow
rule. Do not call `focus`, change selection, remount the composer, or expand
covered content as part of placement.

### Give forced placement priority

Desktop should combine target detection and the transcript-follow decision
in one `useLayoutEffect`. On an update with a forced target, place that target
and skip end-follow for that update. Otherwise follow only when an input that
already required following has changed and the following flag is true. Keep
generation active/phase changes in this decision so progress behavior
continues. Retain the resize observer and have its follow function read the
updated following flag. Move the conversation-change reset of that flag into
the same layout decision, so a later passive reset cannot overwrite placement.

Replace desktop `followTranscript`'s `transcriptEnd.scrollIntoView` with a write
to `transcriptArea.current.scrollTop`, using the container's `scrollHeight` as
the requested end position; the browser clamps it. Use this container-local
operation for initial history placement, ordinary following, and the existing
resize observer. Remove the now-unused `transcriptEnd` ref and sentinel.
ChaWeb already uses a container-local `scrollTop` write; retain that approach.

ChaWeb should extend its existing layout effect in the same way, preserving
the current `layoutKey` handling for composer and viewport size changes.
Include automatic audio, the pending prompt, and the loaded/context props in
the effect inputs, but mode-only and marker-only changes must not scroll.
Consuming a prompt marker can cause another render; it must not invoke fallback
end-follow and overwrite the placement. Keep the last committed follow inputs
(transcript reference and generation fields or `layoutKey`) alongside the
observations in the existing ref to distinguish these cases.

Record observations in the layout effect, not during render. This prevents an
abandoned render from consuming an event and prevents React Strict Mode effect
replays from treating an entry as new twice. Do not leave an independent
end-follow effect that can overwrite the forced target in the same commit.
Record the inputs and the consumed attempt identity in the same observation
ref before calling the parent consume callback. Ignore a marker whose attempt
is already recorded there, even if the parent state update has not committed
yet. Reset that identity with the context. Strict Mode replay then finds no
new event; desktop can also clear its local marker ref immediately.

The two renderers can keep this small decision next to their existing scroll
code. No shared controller or changes to voice generation, playback queues,
native hosts, bridge messages, backend APIs, or database schemas are required.

## Validation and acceptance

Extend `webapp/src/components/LiveChat.test.tsx` for desktop integration and
`webapp/src/chaweb/transcript.test.tsx` for viewport decisions. Use
`webapp/src/chaweb/behavior.test.tsx` to verify the existing automatic audio
setting reaches the behavior. Mock scroll geometry in Vitest; DOM emulation
does not perform browser layout.

Cover these cases:

1. While scrolled up, a new human entry brings its article into view with
   automatic audio both on and off. Verify typed and voice submission use the
   same path; a transcription preview or failed send alone does not scroll.
2. An empty streaming character entry receives its first readable text with
   the same ID and entry count. It forces a scroll only with automatic audio
   on, before completion or audio readiness. Empty/whitespace bodies and a
   removed legacy timestamp do not qualify. Also cover entries first observed
   with readable streaming text or a complete response.
3. Later tokens, a temporary loss and return of rendered text, and completion
   do not force another scroll for that response. A first-readable arrival
   while audio is off is still recorded: enabling audio afterward does not
   catch up. Enabling before any readable text does permit its first reveal.
4. A long response first observed with readable text shows its beginning. A
   short response near the end is clamped into view. Composer focus and
   selection survive. Check the same placement for a long prompt.
5. Token updates and generation progress do not force a reader back after
   they scroll away. The existing end-follow and resize behavior still work
   when the reader remains near the bottom.
6. Replayed snapshots, cache flags, download jobs, playback changes, and
   mode-only toggles do not create a second forced scroll. Initial historical
   content does not count as new. A loaded empty session differs from loading.
7. A local prompt wins when the same commit also contains readable response
   text. With no local prompt marker, multiple candidates produce one scroll
   to the last candidate in transcript order. Each character's first readable
   response can trigger its own reveal on separate commits.
8. Submit the same prompt twice with automatic audio both on and off. Each
   accepted submission targets its visible representative once. Later hidden
   multicast copies do not repeat that jump. Test acknowledgement before and
   after the transcript, an unconsumed/rejected send, uncertain-send
   reconciliation, and a stale callback after a newer send. Setting or
   clearing a marker alone must not scroll.
9. Create a ChaWeb session by sending a long prompt. Its first snapshot focuses
   the prompt even if a long response is already present. Opening that session
   later uses historical end placement. Creation failure, switching vaults,
   or leaving the draft during creation must not focus an unrelated view.
10. Deleting or covering messages does not itself create a new-arrival event.
    Switching sessions or vaults resets the baseline, including when numeric
    entry IDs overlap. Reconnection to the same context retains the baseline.
11. Verify the forced target wins over end-follow in the same commit, including
    Strict Mode, marker consumption, and a resize observer callback after
    placement. Test nested scroll containers: forced placement, initial
    end placement, normal token following, and resize following change only
    the transcript's scroll offset. No path calls `scrollIntoView`.

For implementation, run:

```sh
npm --prefix webapp run typecheck
npm --prefix webapp test -- src/components/LiveChat.test.tsx src/chaweb/transcript.test.tsx src/chaweb/behavior.test.tsx
npm --prefix webapp run build
npm --prefix webapp run build:chaweb
```

Then check real layout in macOS WKWebView, Windows WebView2, and a ChaWeb
browser with a long transcript. Use short and long messages, automatic audio
on/off, manual scrolling, split view, and composer/window resizing. Confirm
that no ancestor or adjacent pane moves. Use existing fake provider/audio
fixtures; this feature needs no live provider API calls.
