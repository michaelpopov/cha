# Block 7: Organization, formatting, and prompt insertion

Status: execution guide; this document does not establish implementation,
approval, or a passing test result.

Split block 7; plan steps 11–12. Next: [block 8](notes-block8.md).

Sources: [implementation split](notes-split.md), [full plan](notes-plan.md),
[detailed design](notes-design.md), [PRD](notes-prd.md), and
[repository instructions](../AGENTS.md). The PRD defines acceptance; for
implementation follow the plan, then design, then architecture as background.
This guide expands the split and does not replace any source step/check.
Report conflicts that this precedence does not resolve.

## Outcome and scope

Implement plan **steps 11–12**. Add rename/move/delete with metadata recovery
and close safety, then connect the approved formatting controls and literal
prompt-to-note insertion. Reuse the existing tree, native API, editor acceptance,
shared composer, save hook, operation slot, and native close attempt field.

## Prerequisites and reading

Complete blocks 1–6, including close verification on each platform used for
this block's close checks. Require approved menus/dialogs/formatting and inactive
selection marker. Read plan steps 11–12; design sections 3.1–3.2, 3.5, 4.3,
7.4, 8, 11.2, and 13; PRD sections 3, 4.3–4.4, 4.6, and 4.8. Windows close
checks remain conditional on that platform completing step 9.

## Implementation sequence

### 1. Complete organization with reliable metadata

Add approved row menus and Rename, Move, Delete dialogs. A menu button first
focuses its row; Shift+F10/menu key opens the same actions. Capture source ID
and vault context when opening a dialog. Focus movement must not retarget it;
vault replacement invalidates dialogs and callbacks.

Move uses Top level plus full folder paths and excludes a moved folder's own
subtree. The core still validates name/parent/cycle rules. Errors preserve rows
and content. Rename/move change only metadata and do not force an editor leave.
Update the active path/accessibility destination when its ancestry changes.

Serialize one mutation including its metadata refresh. Reload and rebuild
only after success, retaining/revealing tree focus. On committed mutation with
failed refresh, retain old rows and report a refresh error; retry only listing.
Mark that listing stale and require fresh metadata before another mutation or
confirmation. In particular, a moved open note must not be treated as unrelated
to a folder deletion based on old ancestry. No optimistic tree, mutation queue,
second listing, or descendant-count API is needed.

### 2. Delete the captured scope under the shared operation slot

Every folder confirmation states that the folder and all contents are removed,
also for an empty folder. A note confirmation names that note. Default focus
is Cancel, with existing dialog focus containment/restoration. Cancel changes
nothing and does not freeze or stop saving.

At confirmation, require current listing/source/context and no competing tree
mutation. Acquire the same operation slot as navigation before determining
whether current metadata ancestry includes the active note. Hold it through
RPC completion and editor disposal/restoration, preventing another note from
opening inside the pending deletion subtree.

For an affected editor:

1. Freeze document changes/submissions; cancel deadline/due save.
2. Settle the one admitted save. Its failure does not block the confirmed
   deletion or open another save dialog.
3. Send `notes.delete` without saving the unsaved remainder.
4. On commit, dispose the editor/remainder, clear its active ID/save error,
   and refresh metadata. Keep the Notes draft; the empty editor state exposes
   New note and disables submission.
5. On delete failure, retain content, selection, history, and save errors;
   restore editing and show the delete error. Submit an uncovered canceled
   deadline/due save immediately. A previously failed save still requires Retry.

For an unrelated subtree, keep editing available but hold the operation slot.
Non-reused IDs/update-only saves and settling admitted work prevent resurrection.
Reject stale completions before altering another editor or vault.

### 3. Integrate deletion with native close

During affected/unrelated deletion, including its admitted-save wait, retain
block 6's deferred attempt ID without a competing flush. The original host
timer continues. After deletion and editor disposal/restoration release the
slot, resume a still-current attempt against the resulting editor. A timed-out
attempt cannot resume after late deletion/save completion; aborting a deferred
close does not cancel the earlier deletion.

### 4. Connect formatting and selection feedback

Connect approved paragraph/headings 1–6, bold, italic, underline, highlight,
bullet/ordered lists, Undo, and Redo to the tested editor commands. Retain the
editor's authoritative selection when toolbar/prompt focus changes. Each
changed formatting action is one isolated accepted history event; no-op actions
add none. Undo/Redo use the existing acceptance/save path.

Show the approved view-only inactive selection/caret decoration when the editor
loses focus, including selected text, images, and Find-selected matches. Derive
it from current editor selection; store no second remembered insertion point.
It must neither save nor change history and must disappear on editor focus.

### 5. Finish and connect literal prompt insertion

Continue block 1's tested command rather than replacing it with a generic HTML
insertion. Capture the originating editor/context and current selection; reject
loading, failed, frozen, or replaced origins. Normalize CRLF/CR to LF and build
one paragraph of literal text/hard breaks, with no inherited/stored marks.
Reject all-whitespace submission using the shared prompt's existing rule.

In one root action, remove only selected content and preserve surviving
prefix/suffix marks, block types, heading levels, and list containers. Apply:

| Selection/location | Required result |
| --- | --- |
| Empty paragraph | Fill in place. |
| Paragraph interior | Prefix paragraph, new paragraph, suffix paragraph; omit empty fragments. |
| Heading interior | Prefix/suffix headings retain level/marks around a new paragraph. |
| List paragraph | Split inside the same item, retaining nested lists and other paragraphs; no extra bullet. |
| Block start/end | Insert before/after in a valid container without empty fragments. |
| Selected image / gap | Replace the image / insert beside it. |
| Cross-block/item text selection | Remove selected blocks only; preserve surviving fragments and containers. Keep insertion in a surviving start item; retain the end item's suffix separately. |

Preserve schema-required list structure. Fit at the nearest surviving valid
boundary if the start container disappears. Fresh-note insertion follows the
end selection, including the last list item or gap after a final image.
Create valid new URL marks, including a final URL without trailing space,
using the shared policy; do not autolink old text or inherit a link. Never parse
prompt HTML/Markdown or execute `/clear`, `/mcast`, or addressing as Chat actions.

Move selection to the new paragraph's end and clear stored marks. Successive
submissions then appear in order before old trailing content. Run the complete
action through exact acceptance and isolated history, retaining prompt focus.
Return `clear_input: true` only after acceptance. Shared prefix clearing retains
later typing/speech; rejected insertion keeps the draft. A later save failure
must not repeat insertion. Undo/Redo do not change drafts or make chat requests.

## Verification

Extend tree/App tests for capture, keyboard menus, duplicates/cycles, rename/
move preserving bodies, refresh-only retries, stale ancestry, and focus/reveal.
Specifically move the active note into a folder, fail refresh, and attempt folder
deletion: confirmation must require fresh metadata.

Test cancel, empty/full-folder deletion, missing IDs, affected/unrelated notes,
pending/failing saves, failed deletion restoring editor and scheduling, retained
draft, late callbacks, and no navigation race or deleted-ID resurrection.

Use structural editor fixtures for every insertion rule, headings 1–6, nested
lists, cross-item selections, marked fragments, images/gaps, multiline text,
final URLs, and successive submissions. Test complete Undo/Redo/selection,
no-op formatting, near-limit atomic rejection, and draft retention. Exercise
Send, Enter, Ctrl+Enter, IME, transliteration, continuous spoken submission,
interleaved typing, and keyed note changes during pending voice/submission.
Run affected frontend/core tests and checks from plan section 5.

## Operator checkpoint

Provide build revision and the block 1 failure-induction method for each
delay or failure. On platforms
with verified native close, run numbered deletion-close cases with expected
results: affected and unrelated deletion; waiting for an admitted save; pending
delete RPC; success/failure resuming the same attempt; duplicate Quit preserving
timer; hung operations reaching both native choices; and late completion after
Cancel close unable to close CHA. Verify confirmation scope, keyboard focus,
formatting, and insertion examples in the actual app. Keep unavailable Windows
cases in the conditional block handoff.

## Completion and handoff

Organization and recovery pass without losing editor state or using stale
ancestry. Deletion-close operator results pass on the required hosts. Formatting
and all prompt structural/history/lifecycle cases pass. Record mutation recovery,
confirmed-deletion behavior, close results, insertion fixtures, and pending
platform checks. Block 8 extends these same editor/history paths.

## Execution and reporting rules

Apply the split's [execution rules](notes-split.md#execution-rules), including
its operator checkpoint, Windows mode, and end-of-block rules, and its
[handoff contents](notes-split.md#handoff-between-blocks). Apply the scope
rules in plan section 1 and the KISS rules in the repository instructions.
