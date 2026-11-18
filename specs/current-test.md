# Current note in desktop Chat — one-time acceptance test

Validate the [PRD](current-prd.md) and [technical design](current-design.md) on macOS (WKWebView) and Windows (WebView2). Use the existing deterministic fixtures for race, failure, wire, and storage assertions. Run the desktop scenario on both hosts to verify the real bridge, window title, composer, and answer together. Check the live Jev classifier separately from CHA’s handling of its decisions.

## Before the timed run

Complete these steps before minute 00. They are not part of the 60 minutes. If a step is not complete when the run starts, mark the affected checks `BLOCKED`.

- **Tested code on both hosts.** The feature is in uncommitted working-tree changes. Move the same tree to the Windows host as a patch or a temporary branch. Record the base revision and a hash of the diff on both hosts.
- **Builds.** Install dependencies, including Playwright for the Windows runner. On macOS, build the test targets listed in A–D, the webapp assets, and a packaged CHA.app from the tested tree, for example with `make package-macos VERSION=<test-label>`. On Windows, run `make build` (it runs `scripts/build-windows.ps1`) and `npm --prefix webapp run build`. Do not use the macOS test host from `make run-native-dev` or `tests/native/macos/run.sh` for F, because it keeps the OS window title fixed at `CHA`.
- **Disposable profiles.** The packaged apps use the default data directory, so prepare each host’s profile under a disposable OS account. Start with an empty vault. Configure an ordinary text provider that works in Welcome and in a two-character forum (Seneca and Marcus below), working Jev settings, and the `debug` level in the `[logging]` section of the application configuration. The configured characters’ actual names can substitute for Seneca and Marcus.
- **Jev settings.** E uses `https://openrouter.ai/api/alpha/decisions` and the default model `typesafe/jev-1.13`. Confirm that the desktop Jev settings use the same URL and model. If they differ, record both and do not use E to explain F decisions.
- **Evidence commands.** Prepare one log search per marker (`Jev request body`, `Jev raw response`, `HTTP model request body`) and per test code (`T-417`, `T-418`, `P-829`).

## Run order and timebox

| Elapsed | Work |
| --- | --- |
| 00–04 min | Check builds, profiles, Jev settings, and access; create the evidence directory |
| 04–12 min | Run A–D automation, the native host scripts, and E classifier evaluation concurrently |
| 12–26 min | Run F desktop scenario on macOS |
| 26–38 min | Run F desktop scenario on Windows |
| 38–54 min | Run G targeted checks and investigate any failures |
| 54–60 min | Record verdicts and clean up |

The 60 minutes include the preflight check, evidence collection, and cleanup, but not the preparation above. Record the tested revision and any working-tree changes, build identity, host OS, and Jev URL and model. If a host, provider, or tool is unavailable at the end of its slot, mark affected checks `BLOCKED` and continue. At minute 54, stop starting cases.

Use synthetic data throughout. Mock R2 and D1 through the existing local tests. Capture `Jev request body`, `Jev raw response`, and `HTTP model request body` excerpts by test marker, omitting credentials and headers. Record response content separately from the request assertions. A lucky answer alone is insufficient evidence that CHA attached the correct note.

## A–D · Deterministic coverage

Run from the repository root on the primary host. Set `qa_dir` to an absolute, run-specific evidence directory outside the repository and create it before invoking the commands. Refresh stale artifacts with `cmake --preset ninja` and `cmake --build --preset ninja --target cha_tests cha_app_tests cha_bridge_tests cha_native_runtime_tests cha_daemon_tests cha_jev_eval_requests`. For desktop assets use `npm --prefix webapp run build` when needed. Use the matching WebView2 build and assets on Windows.

```sh
cases='^(CurrentNoteRouting|JevProtocol|JevProviders|JevRouting|TextInput|WebWireFixtures)\.|^SessionNaming\.(StartsWithJev|StoppedClassification|NamingTimeout)|^ChatSyncTest\.(UnchangedAutomaticNoteCopy|DeletedSource|ClearAndLastTurn)|^BridgeRouterTest\.(DesktopSubmitCaptures|NotesSelection|ReadOnlyNoteWireContract|NotesRejectStaleVaultContext|D1StorageBridgeSaves)|^ApplicationVault\.(UploadIncludesFinalSavedNotes|DownloadReplacesNotes|ConfigurationTransferAndSessionMirror|NotesFollowWholeCopies)|^NativeRuntimeTest\.(StartsWithoutAListener|VaultSwitch)|^ChaWebAdapterTest\.(BootstrapUsesExistingSerializer|NotesReadsReturnOriginalDocuments|CreatesOpensInputsAndStops)'
ctest --test-dir build/ninja -N -R "$cases"
ctest --test-dir build/ninja -R "$cases" -j4 --timeout 45 --output-on-failure \
  --output-junit "$qa_dir/native.xml"
```

```sh
npm --prefix webapp run api-types:check
npm --prefix webapp test -- \
  src/state/bootstrap.test.ts src/state/view.test.ts \
  src/api/nativeClient.test.ts src/api/wireFixtures.test.ts \
  src/components/App.test.tsx src/components/Notes.test.tsx \
  src/components/NotesTree.test.tsx src/components/PromptInput.test.tsx \
  src/notes/useNoteLeave.test.ts src/notes/useNoteSave.test.ts \
  src/chaweb/App.test.tsx src/chaweb/client.test.ts \
  --reporter=default --reporter=junit --outputFile.junit="$qa_dir/web.xml"
```

Run the native host scripts on both hosts before F, because they open native windows. They use their own temporary vaults. `parity.js` checks that `notes.select` stores the note, that opening another conversation keeps it, and that deleting the note clears it. `ui.js` checks the title `CHA: Welcome — UI smoke note` in Settings and the restored editor. On macOS, `make test-ui` builds the assets and runtime and runs `ui.js` at two widths. On Windows, `run.ps1` runs the WebView2 probes; the probe that “enforces CSP and retains session, file, navigation and vault behavior” runs `parity.js`.

```sh
make test-ui
```

```powershell
.\tests\native\windows\run.ps1
```

The items in A–D describe what the listed tests already assert. They are not manual steps. Use them to map failures and identify any assertion that needs direct evidence in F or G. A passing suite establishes the behavior its assertions exercise. The only manual item is marked **Manual, optional**.

### A · Selection, context, and native contract

- `NotesSelectionValidatesSourceIdsAndAppearsInBootstrap`, `ReadOnlyNoteWireContractAndValidation`, and `NotesRejectStaleVaultContext` exercise `notes.select` with `{ "id": id }` and the active epoch. A source note or annotation succeeds with an empty result. A folder, version row, invalid or missing ID, or stale epoch returns the corresponding existing error and preserves the last valid selection. Verify optional `current_note_id` in bootstrap when set and its absence when empty or deleted. `session.submit` still takes the existing text and optional voice usage with session identity. The core supplies selection itself.
- `ApplicationVault` checks show that upload, merge, and config import keep the same ID, including after an epoch refresh. `NotesRejectStaleVaultContext` switches to a copied vault with overlapping IDs, and `DownloadReplacesNotesAndOverwritesOneBackupIncludingOlderSchema2` performs a mocked R2 replacement. Both clear the selection. `ConfigurationTransferAndSessionMirrorExcludeNotes` shows that a reopened application starts with no selection even though the source still exists. `D1StorageBridgeSavesDisconnectsAndNotifiesNotes` covers an applied D1 deletion, and the Notes UI tests cover the refreshed editor and title. `NotesSelectionValidatesSourceIdsAndAppearsInBootstrap` deletes the ancestor of the current source while another note survives. The selection stays empty until an explicit new open.
- The state and Notes tests exercise changing screens, layout and chat, folder and menu focus, source ID retention across text modes, bootstrap restoration, and failed-leave Discard. Discard comes back to the same source and the last saved text. For ChaWeb, the adapter, HTTP client, and app tests cover ordinary note and chat use. **Manual, optional:** if a ChaWeb fixture is already running, open a note then submit an explicit message in a fresh chat. Verify the HTTP submission shape is unchanged, the note opening causes no selection write, and the chat gains no automatic copy.

### B · Send ordering and recovery

- In the Notes split-view fixture, hold a save while submitting an implicit note question and `/mcast Summarize this note`. Verify save starts and resolves before the first submit. Use the unrelated utterance in F and `/clear` in the stale-context case to verify the save applies outside affirmative note requests. A clean or closed editor introduces no save call.
- Exercise deferred in-flight autosave failure alone, the same failure with newer edits queued, and a final-flush failure. Check the composer error, retained draft and edits, restored editability, zero submits, and successful explicit retry. The failed in-flight serial is never retried automatically. With queued newer edits, the next save contains that newer revision. Hold the notes-action slot to get the existing busy error, and change the epoch during a pending save to get `vault_changed`; both leave submit count at zero. A blocked first message leaves the new chat disposable and causes no welcome-turn progression. The `PromptInput` fixture verifies dictation usage survives a `clear_input: false` reply and clears on accepted retry. A failed note save throws an error instead; usage retention on that path rests on code reading only. G checks the selection timing through dictation.

### C · Jev request shape, recipient independence, and cancellation

For each row, assert the Jev call count and exact question keys before checking generation or transcript. For `/mcast` count the classification once for the whole batch.

| Input | Selection | Jev questions and expected routing |
| --- | --- | --- |
| Implicit prompt | Empty | One `recipient` question. Existing default and fallback behavior. |
| Implicit prompt | Set | One request with `recipient` and `use_current_note`. `undefined` preserves the captured default. A valid named or all-characters choice controls routing. |
| `@Seneca`, `@-`, `/mcast` | Set | One `use_current_note` question. The chosen recipient(s) and existing default stay fixed. An unsolicited recipient choice has no effect. A self-note attaches on `yes` and starts zero answers. |
| `@Seneca`, `@-`, `/mcast` | Empty | Zero Jev calls. Dispatch follows the ordinary explicit path. |
| Standalone Ref ID, absolute file path, `/clear`, invalid `@Name` | Set | Zero Jev calls. Ref ID and path attach the requested item, `/clear` clears the conversation, and an invalid handle returns its existing error. |

`JevProtocol` checks that state carries prompt and selected name. Recipient options belong to the recipient question. Note bodies, other note titles, and history are absent. Exercise `no`, a missing answer, wrong type, and an unknown choice. Each skips attachment without a note-decision notice, and a valid recipient still routes correctly. An invalid recipient with an exact `yes` keeps the existing recipient fallback notice and still attaches. `yes` supplied for an unasked question has no effect. An affirmative `self_note` decision and `undefined` with Self-notes already active both attach without a character request.

Use `CurrentNoteRouting`, `JevRouting`, `JevProviders`, and `SessionNaming` for deferred dispatch. Disabled Jev, timeout, and transport or unreadable-response failures use the usual route without auto-attachment. An explicit recipient waits for classification when enabled and current, and gets no recipient fallback notice on classification failure. Stop, cancellation, or expiry while pending produce no late turn, attachment, or answer. Naming may run in parallel and uses the prompt and prior copies, with no request-only note line. Generation waits for naming and classification to finish on a first message.

### D · Copy, projection, and source failures

- Use `CurrentNoteRouting` and `TextInput` with different markers in full, compressed, L1, and L2 text. The saved compressed Markdown wins when present, including when the editor shows L1 or L2. Full saved Markdown wins when compressed is absent. Count UTF-8 Markdown bytes after choosing the source. Accept 262,144 bytes including a multibyte character and reject the same text plus one ASCII byte. `CurrentNoteRouting` has the exact boundary fixture and tests oversize after admission with an older copy present.
- Capture a positive send, then change selection while the Jev response is held with copies of both notes already attached. Read the originally captured source ID. If it was renamed while pending, read its latest saved name and text and use that name in the notice and request. When a send targets multiple characters, count one copy refresh and one notice before the human turn, with the same note name supplied for every target. `AllCharactersRefreshTheCopyOnceAndNameTheNoteInEveryRun` covers an implicit all-characters send with a changed copy and a selection change while classification is pending. The `/mcast` check covers an unchanged copy.
- Compare journal entries and sync records. First use adds one copy and `Added note …` notice. A name-only or text-only edit updates that row and emits one `Updated this chat’s copy of note …` notice. Unchanged name and Markdown produce zero copy writes, notices, and attachment sync records. The note line is still present in the answer request. `UnchangedAutomaticNoteCopyWritesNoNoticeOrSyncRecord` measures the write and sync behavior. Apply a Ref ID twice, including when another note is selected, and verify it always refreshes the referenced item and records its notice.
- Inspect answer input and the stored transcript. The last user message alone ends in `The user refers to the note “Trip plan”.` for an affirmative send. The human entry keeps the original prompt content, with addressing represented as in ordinary chat. Earlier turns and replies remain unchanged. The system context keeps the usual `# Note: …` headings for all attached copies. Subsequent requests still carry the copies, but omit the earlier request’s note line. Reopen the chat and delete the original source to confirm follow-up still sees the saved copy. A separate chat begins without those copies. `/clear` removes them from that chat’s future requests.
- While classification is held, delete the selected source or replace its text with oversize Markdown. Leave an older copy in the chat, and keep another note available. Expect respectively `That note was not found.` or `This note is too large to add to a chat. The limit is 256 KiB of text.` The result retains the originating draft, changes neither the old copy nor the recipient default, and starts zero human turns, attachment notices, or character requests. Use `DesktopSubmitCapturesTheCurrentNoteAndRetainsDraftOnMissingSource` for the bridge result. The damaged-document and journal rollback `TextInput` tests cover the shared attachment path. Check the same failure invariants if a corrupt read or journal failure is observed during an automatic send. Retry deliberately after selecting a valid source or shortening the text.

## E · Live classification

Use an authorized `JEV_EVAL_API_KEY` already available in the test environment. The following selection runs the 11 current-note fixture cases and four scenario prompts from F and G, five samples each, with the existing evaluator’s five-second per-request timeout. The scenario prompts give a live baseline for the decisions that F and G expect. The classifier starts independently of the deterministic suites. If credentials are unavailable, use `--requests-only` in the second command to validate request shape and record live choice accuracy as `BLOCKED`.

```sh
python3 - "$qa_dir/jev-cases.json" <<'PY'
import json
import sys
from pathlib import Path

fixtures = json.loads(Path('tests/fixtures/jev/cases.json').read_text())
selected = [case for case in fixtures if 'current_note_name' in case]
assert selected
# Scenario prompts from F3 and G1, as Jev receives them after explicit addressing is parsed.
note = 'Hotel plan'
selected += [
    {'roster': [], 'current_note_name': note,
     'prompt': 'Quote the cancellation clause in this note.', 'expected': {'use_current_note': 'yes'}},
    {'roster': [], 'current_note_name': note,
     'prompt': 'Quote the cancellation code in this note.', 'expected': {'use_current_note': 'yes'}},
    {'roster': [], 'current_note_name': note,
     'prompt': 'Buy stamps.', 'expected': {'use_current_note': 'no'}},
    {'roster': ['Seneca', 'Marcus'], 'current_note_name': note,
     'prompt': 'Everyone, quote the cancellation code in my current note.',
     'expected': {'recipient': 'all_characters', 'use_current_note': 'yes'}},
]
Path(sys.argv[1]).write_text(json.dumps(selected, ensure_ascii=False), encoding='utf-8')
PY
python3 scripts/evaluate-jev.py --cases "$qa_dir/jev-cases.json" \
  --output "$qa_dir/jev-results.json"
```

Compare each usable choice to the case’s `expected` value for each question. Positive examples include “the note I just opened,” the selected title, a natural addressee, questions after explicit addressing has been parsed, and “Everyone, …” with `all_characters`. Negative examples include another note’s title, drafting a thank-you note, an unrelated self-note with and without a fixed recipient, revising the previous answer, and ambiguous “Summarize this.” Record mismatches as classifier deviations with counts per prompt. Record transport or service errors separately. Do not rerun mismatches to improve the reported result. A mismatch on a scenario prompt explains the matching F or G deviation in advance.

## F · Desktop scenario on each host

1. **Empty selection.** In an empty vault, send `@- Empty vault smoke`. It records a self-note and makes no Jev call. While a chat is open the OS window shows `CHA: <chat>`. In Notes create a `Travel` folder with **Trip plan** containing `Free cancellation through Friday at 17:40, code T-417.` Create **Packing list** outside that folder containing `Shoes: red boots, code P-829.` Keep a copy of its Ref ID for later. Use full text for this scenario. Open Trip plan last. Opening it shows `CHA: <chat> — Trip plan` and adds nothing to the chat.
2. **Navigation and first attachment.** In single view, focus the folder and open then dismiss the Packing list item menu without opening its editor. Visit Settings and Events, reopen Notes, then toggle split view and back. On each host, send `Quote the cancellation clause in my current note.` in Welcome and check for an added notice and an answer with `T-417`. On each host, open or create a chat named `Travel QA` in the two-character forum. Trip plan still opens in Notes and appears in the native title across the screens. Send `Seneca, quote the cancellation clause in my current note.` Expect one added notice before the prompt, one Seneca reply with `T-417`, and the note line in the last user message of the captured request.
3. **Explicit send and refresh.** In split view rename Trip plan to **Hotel plan** and move it to the root. Check title and editor selection. Keep the renamed note editable and replace the code with `T-418`. Immediately send `@Marcus Quote the cancellation clause in this note.` Expect the saved code `T-418`, one update notice, the new title in the request, and only Marcus answering. Send `/mcast Quote the cancellation code in this note.` Expect one request per character with `Hotel plan` in each, zero additional copy notices and the same default target after the explicit turns. Add `Desk is in the north lobby.` to the note and immediately send the unrelated `@- Buy stamps.` Verify the new sentence is saved, a self-note is recorded, and no copy update or reply occurs.
4. **Copy lifetime.** Open Packing list and send `Make that answer shorter.` The classifier should skip a new copy and leave the existing Hotel plan copy available. Reopen **Hotel plan**, then attach Packing list using its Ref ID and confirm exactly the referenced copy is added. Delete Hotel plan with Packing list still in the vault. The title drops its note part without selecting Packing list. Open another chat, then reopen `Travel QA` and ask `@Seneca What hotel cancellation code was in Hotel plan?` The captured request still contains the saved Hotel plan copy with `T-418` and omits the request-only note line. Send `/clear`, then ask a generic question. Both note headings are gone from the request. Open Packing list, quit the process, and relaunch the same profile. The selection is empty even though Packing list still exists.

Capture the title on each host and a transcript/payload pair from the affirmative turn. If a step diverges, record the first transition that differs and retain the originating Jev decision where applicable. Use G to check any required branch that wasn’t observed.

## G · Targeted checks and triage

1. **All characters and one copy refresh (4 min).** Use a scratch chat in the two-character forum. Create a new Hotel plan with code `T-417`, attach it and Packing list by Ref ID, open Hotel plan, and save a change to `T-418`. Send `Everyone, quote the cancellation code in my current note.` Verify a single Jev call with `all_characters` and `yes`, one update notice, and both answer inputs naming Hotel plan and containing the new code. `AllCharactersRefreshTheCopyOnceAndNameTheNoteInEveryRun` in D covers CHA’s handling of this decision. If Jev does not choose `all_characters` and `yes`, record a classifier deviation. Compare with the existing held explicit-recipient and `/mcast` checks in D.
2. **Dictation boundary (3 min).** Run this only if dictation is configured; otherwise mark it `NOT RUN`. Hotel plan is open in split view and has no unsaved edits. With unsaved edits, the note switch holds the notes-action slot, and the Send can fail with “A notes action is already in progress.” Dictate “Quote the code in my current note” and press Send. While the dictated send is finalizing, switch to Packing list. Check that the finalized text is submitted with Packing list. The finalizing window is short. If the switch lands after the submit, record the timing; the request then correctly names Hotel plan. Use the held-classifier check in D for the opposite boundary after submit.
3. **Two instances (4 min, macOS).** Choose **Window → New Instance**. In instance A open Hotel plan, and in instance B open Packing list. In each instance open a different scratch chat in the two-character forum and send `@Seneca Quote the code in my current note.` Verify that each title shows its own note and that each request names its own note and code. In B, delete Hotel plan. In A, verify that the title drops its note part, then send the same message again. Expect no Jev call, no new copy notice, and no note line. Record the first step that differs, then quit instance B.
4. **Four title states (2 min).** Reuse the fixtures in `Notes.test.tsx` for the no-open-chat cases that a normal launch through Welcome rarely exposes. Verify `CHA`, `CHA: Trip plan`, `CHA: Travel QA`, and `CHA: Travel QA — Trip plan` while visiting Chat, Notes, Settings and split view as applicable. Use the real OS title in F for the two states with a chat open, and `document.title` for the empty/failed-open fixtures. The test vault’s distinct name is absent in all four states, and rename is reflected without switching away.
5. **Triage (3 min).** Prioritize wrong-note attachment, loss of a draft, unexpected provider requests, and stale state crossing a vault boundary. Distinguish a wrong Jev decision from a correct decision applied to the wrong source or recipient. Capture a minimal repro before altering the fixture. Keep any temporary fixture adjustments outside the tested checkout or revert only the probe changes when done.

## Verdict and cleanup

Record `PASS`, `FAIL`, `BLOCKED`, or `NOT RUN` for A–G, with separate F results for macOS and Windows. A partial case must name its unverified assertions. Include build identity, duration, test summaries, classifier counts, and sanitized payload or transcript evidence. For each failure record the prompt, selection at submit, expected and observed behavior, and a reproduction. A missing host, credential, or usable fixture is a coverage gap. Accepted behavior includes waiting for the configured Jev deadline on explicit messages, keeping accumulated copies until `/clear`, and ambiguity between copies with the same title.

Report these known coverage gaps in the verdict:

- No test covers opening another note while Send saves edits, and the save is too short to reach by hand. By code reading, `leaveNoteThen` in `webapp/src/notes/useNoteLeave.ts` ignores a note open while Send holds the notes-action slot. Report this as an open product question, not as a test failure.
- Dictation usage after a thrown save error is retained by code reading only (B).

Before minute 60, close the test apps, including both macOS instances, remove temporary test profiles and any probe state, and preserve the sanitized evidence. Leave the tested source and the operator’s normal profiles untouched.
