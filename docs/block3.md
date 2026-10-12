# Block 3: Shared prompt and independent drafts

Status: execution guide; this document does not establish implementation,
approval, or a passing test result.

Split block 3; plan step 6. Next: [block 4](notes-block4.md).

Sources: [implementation split](notes-split.md), [full plan](notes-plan.md),
[detailed design](notes-design.md), [PRD](notes-prd.md), and
[repository instructions](../AGENTS.md). The PRD defines acceptance; for
implementation follow the plan, then design, then architecture as background.
This guide expands the split and does not replace any source step/check.
Report conflicts that this precedence does not resolve.

## Outcome and scope

Implement plan **step 6**. Extract the existing composer into controlled
`PromptInput`, lift one Chat and one Notes draft into App, and preserve Chat
behavior. Keep this change separate from editor integration; add no temporary
Notes UI just to demonstrate it. Existing composer appearance must remain.

## Prerequisites and reading

Read blocks 1–2 handoffs, plan step 6 in full (the authoritative component
contract), design sections 4.1/4.3, and PRD sections 3/4.6. Inspect current
ChatScreen voice/transliteration/submission code and tests before extraction.
This block can proceed while Notes visual approval is pending if it preserves
the existing Chat UI. Any new visible proposal still requires approval.

## Main code paths

Use new `webapp/src/components/PromptInput.tsx`, existing `ChatScreen.tsx` and
`App.tsx`, `LiveChat.test.tsx`, and current voice/transliteration helpers/tests.
Introduce no Chat/Notes mode protocol or new speech subsystem.

## Implementation sequence

### 1. Keep the plan's full component contract

Use the exact `PromptInputProps` and `PromptVoiceControls` definitions from
plan step 6; do not reduce the contract to a textarea plus submit callback.
Responsibilities are:

| Contract | Responsibility |
| --- | --- |
| `value`, `onChange` | Controlled draft with the existing synchronous async-read ref. |
| `enabled` | Text entry and voice cleanup when availability changes. |
| `submitEnabled`, `microphoneEnabled` | Independent submission/microphone availability. |
| Labels/placeholder | Parent-supplied existing Chat copy or approved Notes destination. |
| `client` | Existing voice runtime/transport access only. |
| `onSubmit` | Parent handler returning `{clear_input: boolean}` after acceptance. |
| `onSubmittingChange`, `onError` | Parent pending-state/error integration and clearing. |
| `stopAction` | Chat-owned Stop button replacing Send and blocking submit while present. |
| `formControls` | Chat target selector inside `cha-composer-controls`, before microphone/actions. |
| `statusRow` | Complete parent-owned existing lower row with supplied transliteration/voice controls. |

Extract text entry, resizing, transliteration, voice lifecycle, and serialized
submission using current markup/styles. Chat retains commands, addressing,
generation rules, target selection, Stop handler/availability/error handling,
forum/From/To/speaker/picture/sidebar row content and ordering. The shared
component receives no session snapshot, target list, generation state, or mode.
Notes later omits form controls/Stop and supplies its approved reduced row.

Chat passes text availability from `sessionAvailable`, with current action/
connection conditions separately. Retain empty-input, voice-transition, and
pending-submission checks in the shared component. Stop is `type="button"`,
with independent availability, and must work when sending is unavailable.
Missing voice configuration disables microphone only; typed entry still works.
A leave freeze later blocks Notes submission while preserving typing/dictation.

### 2. Lift exactly two App-owned draft slots

Keep Chat `{key, text}`, where key includes vault/forum/session identity, and
one Notes text string. Returning to the same conversation after Notes restores
Chat draft. Only successful opening of a different conversation replaces the
Chat slot; failed opens retain it. Notes draft survives note/panel changes.
Clear both drafts only on successful explicit vault switch, not attempted/
canceled/failed switching or every maintenance epoch change. Full page reload
starts empty. Add no per-conversation/note map or persistent draft storage.

### 3. Preserve synchronous draft reads and accepted-prefix clearing

Initialize `draftRef` from controlled value, synchronize external committed
value changes, and route every local edit through `updateDraft(next)` updating
the ref before `onChange`. Transcription callbacks, phrase removal, Ctrl+Enter,
and reads after await use the ref, not captured props. This is the existing
async-read mirror, not another draft store.

Notify the parent's submitting state from start through transcription finalizing
and submission completion. Chat keeps target/cover/delete disabled throughout
this interval. Release notification during cleanup and ignore old-instance late
completion. Keep current error presentation/clearing through `onError`.

After accepted submission, read current ref and remove only the still-matching
submitted prefix with the existing whitespace rule. Later typing/speech stays;
changed nonmatching input remains. Rejection/failure preserves draft. Retain
Enter submit, Ctrl+Enter line break, and IME composition Enter behavior.

### 4. Preserve voice and Stop behavior

Keep existing manual submit: finish transcription, stop microphone, submit the
completed draft. Microphone stop finishes text without submitting. Configured
spoken-send removes the phrase, submits while listening, and continues recording.
Do not add pause-send, a Notes voice mode button, or audio-response policy.
The narrow voice controls let Chat retain speaker/dictation coupling in
`toggleAutoAudio`.

While Chat supplies `stopAction`, replace Send and block form/Enter submission;
retain the existing pending spoken-send state. After Stop disappears, apply
existing availability checks and delay, then submit once without the phrase,
with recording still active. Preserve independent editing/dictation meanwhile.

When `enabled` changes, use the current voice cleanup: abort startup, cancel
recording/transcription, clear pending phrase/timer, and ignore stale callbacks
without clearing the App draft. Session availability can change without a
conversation key change; cleanup must still occur.

### 5. Use lifecycle cleanup for origin protection

Keep existing composer/voice attempt checks after awaits and before invoking
captured handlers or changing drafts. Unmount aborts voice startup, cancels
transcription/pending voice timer, releases parent submitting state, and
invalidates continuations. It cannot undo an already accepted action/admitted
request.

Notes later mounts with React `key={activeNoteId}`; the key is not a component
prop. Remount cleans old note work while App preserves its Notes draft. Remove
shared mount/conversation effects that clear draft on remount. Vault replacement
must dispose the subtree even if note IDs match. Add no note-ID invalidation
protocol inside PromptInput.

## Verification

Run existing Chat regressions and extend `LiveChat.test.tsx` plus focused shared
prompt tests. Use controlled transcription/submission promises and fake clocks.
Cover:

- Chat commands/addressing, target selector in its existing form, Send/Stop,
  pending guards, status-row order, error clearing, speaker/dictation coupling,
  transliteration, resize, Enter/Ctrl+Enter/IME, and typing without voice setup.
- Deferred transcription/submission with later edits/speech: reads are current,
  accepted prefix only clears, rejected/nonmatching drafts remain, pending
  controls stay guarded through finalization, and Stop works without Send.
- Spoken phrase during generation: no submit while Stop exists, exactly one
  delayed phrase-free send after removal, recording remains active.
- Start recording with pending spoken-send; make `sessionAvailable` false
  without changing key, then restore it and advance beyond delay. Recording
  canceled, stale transcript ignored, draft preserved, and phrase never sends.
- Test keyed A→B remount through the component contract with unresolved
  submission and voice timer. Late A work neither submits into B nor clears
  shared draft. Unmount pending cleanup releases parent state.
- App-owned draft transitions: same/different/failed conversation opens,
  note/panel remount retention, successful versus failed/canceled vault switch,
  and maintenance context change that is not a user switch.

Run affected frontend suites/typecheck, then check and both builds from plan
section 5. No production Notes UI is needed for these tests. Where real voice/
IME interaction is required, distinguish unrun operator checks from automated
coverage; include them in the later packaged checkpoints.

## Completion and handoff

Chat behavior and visuals remain intact with passing regression tests. Notes
can supply an accepted-insertion handler without live Chat or configured voice
for typed input. Record the exact props/slot ownership, draft-reset rules,
submission/voice cleanup evidence, and remaining live-provider checks. Block 4
builds the editor/save foundations; block 5 mounts the keyed Notes instance.

## Execution and reporting rules

Apply the split's [execution rules](notes-split.md#execution-rules), including
its operator checkpoint, Windows mode, and end-of-block rules, and its
[handoff contents](notes-split.md#handoff-between-blocks). Apply the scope
rules in plan section 1 and the KISS rules in the repository instructions.
