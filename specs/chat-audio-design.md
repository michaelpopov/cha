# Chat audio playback speed

Date: November 4, 2026

Status: Proposed technical design. This document does not implement the feature.
The toolbar sketch below is the proposed visual design for review.

## Purpose and scope

Add controls to the desktop Chat toolbar to decrease and increase audio playback
speed. Use the same range, step, and default as Notes audio. A change must apply
to the current audio without restarting it, and to later chat audio, including
audio in other sessions.

Cover macOS WKWebView and Windows WebView2, including manual message playback,
automatic response playback, cached audio, and audio played while generation is
still in progress. Use the existing audio player and download paths.

The separate ChaWeb conversation screen, Notes controls, voice previews, and
provider voice-generation settings are outside this change. The shared player
must retain normal-speed behavior for callers that do not specify a rate.

## Current implementation

| Area | Existing behavior |
| --- | --- |
| `webapp/src/components/NotesScreen.tsx` | Owns `playbackSpeed`, initially `10`. Its Playback speed menu offers Decrease speed, Increase speed, and Default speed. The integer values are `5`, `10`, `15`, and `20`, converted to playback rates by dividing by ten. `NotesScreen` stays mounted when another note opens, so the selected speed stays for all notes while the Notes pane is open. |
| `webapp/src/notes/useNoteAudio.ts` | Sets both `defaultPlaybackRate` and `playbackRate` on each audio element, and updates the current element when the selected rate changes. Keeps the latest rate available to asynchronous clip loading. |
| `webapp/src/components/ChatToolbar.tsx` | Renders Character, Model, and Effort selectors. Currently returns no toolbar when the selected character has no settings. |
| `webapp/src/components/ChatScreen.tsx` | Owns the active `TextToSpeechSession` ref. Manual and automatic playback both use `playAudio`. The toolbar selectors are disabled during generation, disconnection, and pending actions. |
| `webapp/src/textToSpeech.ts` | `TextToSpeechSession` creates the HTML audio element for complete blobs and MediaSource/ManagedMediaSource streams. It handles resume positions, completion, input suppression, and resource cleanup, but has no playback-rate option or setter. Automatic MP3 playback adds 2.5 seconds of encoded silence at the start of a clip so that the output device can start. |
| `webapp/src/components/App.tsx` | Keys `ChatScreen` by vault, forum, and session, so opening another session mounts a new chat view. While a session opens, the chat panel shows `SessionOperationState` instead of `ChatScreen`. `Screen` owns the split-view `ratio` state and stays mounted across session changes, vault changes, and view changes. It mounts again only when bootstrap starts again. |

Playback speed is a local media property. It must not change
`TextToSpeechVoice.settings.speed`, which controls audio synthesis by the
provider. It must not generate another audio request or alter cached bytes.

## Proposed controls

Append one compact group after Effort in the existing Chat toolbar. The group
has two buttons and the current rate as plain text between them:

```text
[ Character       ▾ ]  [ Model       ▾ ]  [ Effort   ▾ ]  [ − ] 1.0× [ + ]
```

At a narrow width, wrap the whole group onto the next toolbar line using the
existing flex layout:

```text
[ Character       ▾ ]  [ Model       ▾ ]  [ Effort   ▾ ]
[ − ] 1.5× [ + ]
```

Do not add a panel title, legend, visible helper text, menu, reset button, or
settings page. Use the existing button appearance, focus treatment, and
platform sizing. Use plain minus and plus glyphs. Give the rate text a fixed
width to avoid movement when the rate changes. The rate text is not a button
and is never disabled. Keep the group together with inline flex and no
internal wrapping. It participates in the toolbar's existing wrap behavior; it
is not pinned to the window edge.

| Control | Action | Accessible name and tooltip | Disabled condition |
| --- | --- | --- | --- |
| `−` | Decrease by 0.5× | Decrease speed | Rate is 0.5× |
| Rate text, such as `1.5×` | None | Not applicable | Never |
| `+` | Increase by 0.5× | Increase speed | Rate is 2.0× |

The button labels are the Notes menu labels. Expose the controls in a group
named `Playback speed`, the Notes menu label. Use native `button` elements with
`type="button"`. No new global keyboard shortcuts are needed. To return to
1.0×, use the decrease or increase button; it takes at most two clicks.

The group follows the existing Chat toolbar visibility setting. It is shown
whenever that toolbar has a snapshot, even if no character settings are
available. Keep the existing character-settings condition around the three
selectors only; audio rate does not depend on a selected character.

Do not apply the selectors' `disabled` prop to the speed group. Speed changes
are local and remain available during generation, pending actions, connection
loss, and audio loading. They are also available before playback and when
voice-output configuration is absent: cached audio can still be played. Rate
bounds are the only button disable rules.

## Playback behavior

| Situation | Required result |
| --- | --- |
| The application starts, or bootstrap starts again | Start at 1.0×. |
| Decrease or increase is selected | Move through 0.5×, 1.0×, 1.5×, and 2.0×, without passing either bound. |
| Audio is playing | Apply the rate to the existing audio element without changing its source, media position, or play/pause state. Speech pitch stays the same through the media element's default pitch preservation. |
| Audio is loading, or a download acceptance callback is pending | Store the selection immediately. The eventual player starts at the latest selected rate. |
| Automatic playback advances to another response | Use the latest selected rate. Keep transcript order and the current queue behavior. |
| A message is stopped and played again | Use the current rate and the existing saved media position. |
| Playback ends, fails, or loses audio focus to Notes | Retain the selected chat rate for later chat playback. |
| Automatic audio is toggled, cache is cleared, toolbar is hidden, or the session reconnects | Retain the selected rate. |
| Another session opens, the vault changes, or you leave Chat and return | Retain the selected rate. |

Notes keeps its speed while you open other notes. Chat keeps its speed while
you open other sessions. Keep the selection in the `Screen` component of
`App.tsx` for the application run. Do not save it across application restarts.
Do not add a per-session map, local storage, database field, or reducer state.

Keep Notes and Chat speed selections independent, including in split view.
Their existing shared audio-focus behavior still determines which audio plays.

Resume positions remain media seconds, not elapsed listening time. For example,
stopping at media position 12 seconds at 2.0× still saves approximately 12
seconds, using the existing leading-silence adjustment. Do not multiply or
divide resume positions by the rate.

Automatic playback's leading silence keeps its wall-clock duration of 2.5
seconds at the rate that the session has when it creates the silence. Without
this, the silence lasts 1.25 seconds at 2.0×, and a slow output device can lose
the first spoken words; at 0.5× it lasts 5 seconds. A rate change during the
silence changes only the remaining silence. No separate silence timer or
buffering policy is added. Faster playback can reach the end of available
streaming audio sooner; retain the current waiting and error behavior.

## Technical changes

### Own the selection in Screen

Add `playbackSpeed` state initialized to `10` in `Screen` in `App.tsx`, next
to the existing `ratio` state. Use the same integer units as Notes. Pass the
value and its setter through `ScreenContent` to `ChatScreen`. `ChatScreen`
passes the value and `onPlaybackSpeedChange(nextSpeed)` to `ChatToolbar`.

In `ChatScreen`, use one change handler for both buttons. It stores the value
that it gets: update the latest-speed ref, call the setter from `Screen`, and
call `textToSpeechSession.current?.setPlaybackRate(nextSpeed / 10)`. The
toolbar supplies only values inside the range, so the handler does not check
the bounds again. This handler must not call `stopSpeech`, start playback,
submit a download, change the auto audio toggle, or send a session-settings
command.

Keep a ref with the latest selected speed. Initialize it from the prop when
`ChatScreen` mounts, and update it synchronously in the change handler, before
the state update. Use that ref when `playAudio` constructs a session. This
small mirror is needed because `toggleSpeech` can call `playAudio` from a
download promise created by an earlier render: the captured value can then be
stale. Do not add an effect that resets speed on session, vault, conversation,
or audio-cache changes.

There are two distinct pending cases:

1. Before a session exists, `playAudio` reads the latest speed ref when it
   eventually creates one.
2. After a session exists but before its audio element exists, its setter
   retains the latest rate until element creation.

Both manual and automatic playback already enter `playAudio`; extend that one
construction site. Do not create separate rate paths for those modes.

### Extend TextToSpeechSession

Add this small interface to `webapp/src/textToSpeech.ts`:

```ts
interface TextToSpeechSessionOptions {
  // Existing options remain.
  playbackRate?: number; // Omitted means 1.0.
}

class TextToSpeechSession {
  // Existing members remain.
  private playbackRate: number;
  setPlaybackRate(rate: number): void;
}
```

Initialize the private rate from `options.playbackRate ?? 1`. Chat supplies
`latestSpeed.current / 10`. Supported Chat values are the four rates specified
above; no configurable range or provider capability detection is needed.

The setter stores the rate and, if an element exists, sets both
`audio.defaultPlaybackRate` and `audio.playbackRate`. It does not call `play`,
`pause`, or `load`, and does not change `currentTime`.

In `createAudio`, after the existing source assignment and `audio.load()` reset,
apply the stored default and current rates before playback starts. Do not set
`preservesPitch`; its default already keeps the pitch, as in Notes. This covers
newly created and caller-supplied audio elements, complete blobs,
stream-to-blob fallback, raw MP3 MediaSource, and wrapped MP3 streams. All
these paths already converge on `createAudio`; do not modify their fetch,
decode, or buffer loops.

Use one private application method for those property assignments, called by
element creation and the setter. The setter must work before `play`, while
source resolution is pending, and while the audio element is paused. It must
not create an element or acquire audio focus by itself. A call after `stop`
must not revive playback or allocate resources.

Existing callers in voice previews, character previews, and ChaWeb omit the
option and continue at 1.0×. `createAudio` always applies the stored rate, also
to a supplied audio element.

In `silentFrames`, calculate the frame count with the stored rate:
`Math.ceil(2500 * this.playbackRate / frame.duration)`. Keep the existing
`leadingSilenceSeconds` calculation from that count, so saved positions still
exclude the silence.

Keep completion, playback-position reporting, native resource release,
microphone suppression, echo-tail handling, and playback-focus ownership in
their current paths. Rate changes must not simulate pause, completion, or
another audio start in `speechPlayback.ts`.

### Keep the toolbar presentational

Extend `ChatToolbar` with the speed value and change callback. Render the new
group outside the conditional character selectors, replacing the current
whole-component early return when character settings are absent. Compute
decrease/increase targets with the same bounded integer arithmetic as Notes.

Give the group the existing `cha-notes-toolbar-group` class. It already
provides an inline-flex group without internal wrapping and the square button
size. The existing Windows rule for `.cha-chat-toolbar` already sets the same
button-size variables. In `webapp/src/styles/app.css`, add only one rule for
the fixed width of the rate text. Do not extract the Notes editor's
`ToolbarMenu`, introduce a general playback-controls component, or change the
Notes menu for this feature.

### Change boundaries

| File | Intended change |
| --- | --- |
| `webapp/src/components/App.tsx` | Own the speed state in `Screen` and pass it through `ScreenContent` to `ChatScreen`. |
| `webapp/src/components/ChatScreen.tsx` | Receive the speed, handle changes, pass toolbar props, and initialize each playback session at the latest rate. |
| `webapp/src/components/ChatToolbar.tsx` | Add the two-button group with the rate text and render it independently of character-settings availability and selector disabling. |
| `webapp/src/styles/app.css` | Add one rule for the fixed width of the rate text. |
| `webapp/src/textToSpeech.ts` | Add an optional initial rate, a live setter, rate application in `createAudio`, and the rate in the leading-silence frame count. |
| `webapp/src/components/ChatToolbar.test.tsx` | Extend control and disabled-state coverage. |
| `webapp/src/components/LiveChat.test.tsx` | Cover the pending download and automatic playback rates. |
| `webapp/src/textToSpeech.test.ts` (new) | Test the setter before and after audio-element creation on the blob path. |

No changes are required to the native hosts, backend, bridge protocol,
`resources/dto.yaml`, generated API types, audio download manager, cache keys,
stored audio, or provider requests. Do not update `docs/` for implementation.

## Validation and acceptance

For toolbar tests, verify:

1. The existing three selectors retain their behavior. The speed group has
   two buttons and shows the rate with one decimal place plus `×`.
2. Repeated decrease/increase clicks reach the correct bounds; the appropriate
   bound button is disabled.
3. Disabling the selectors does not disable speed controls. Missing character
   settings still leaves the speed group available.

For session and Chat integration tests, verify:

1. On the blob path, a setter call before audio-element creation reaches the
   element before `play()`. A setter call after creation updates both rate
   properties and does not call `play`, `pause`, or `load`, or change
   `currentTime`.
2. A download that completes after a speed change plays at the latest speed.
3. The next automatic reply starts at the selected speed.

Use controlled resource promises and a fake audio element in Vitest. Exercise
`TextToSpeechSession` itself for its tests; a mock class alone cannot verify
media behavior. DOM emulation cannot verify audible speed, pitch, real
streaming support, or toolbar layout.

For implementation, run:

```sh
npm --prefix webapp run typecheck
npm --prefix webapp test -- src/components/ChatToolbar.test.tsx src/components/LiveChat.test.tsx src/textToSpeech.test.ts
npm --prefix webapp test
npm --prefix webapp run build
npm --prefix webapp run build:chaweb
```

The full frontend suite checks other users of the shared player, including
Notes, previews, and ChaWeb. Use existing fake provider/audio fixtures; no live
provider API calls are required.

On both native hosts, listen to a sufficiently long clip at 0.5×, 1.0×, 1.5×,
and 2.0×. Change rate during playback, loading, automatic response generation,
and cached replay. Confirm audible speed changes without a pitch shift or a
restart, normal Stop/resume behavior, ordered automatic replies, and unchanged
microphone suppression. Confirm that automatic replies at 0.5× and 2.0× start
after about 2.5 seconds and keep their first spoken words. Open another session
and confirm that the rate stays. Check cached playback without configured voice
output. Check the toolbar at narrow widths and in split view, including
disabled selectors, toolbar visibility, keyboard focus, and platform button
sizing.
