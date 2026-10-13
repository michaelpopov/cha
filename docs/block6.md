# Block 6: macOS termination selection and native close

Status: execution guide; this document does not establish implementation,
approval, or a passing test result.

Split block 6; plan step 3 followed by step 9. Next: [block 7](notes-block7.md).

Sources: [implementation split](notes-split.md), [full plan](notes-plan.md),
[detailed design](notes-design.md), [PRD](notes-prd.md), and
[repository instructions](../AGENTS.md). The PRD defines acceptance; for
implementation follow the plan, then design, then architecture as background.
This guide expands the split and does not replace any source step/check.
Report conflicts that this precedence does not resolve.

## Outcome and scope

Implement plan **step 3, then step 9**, in that order. Select one tested macOS
termination strategy, then protect normal close using the real Notes editor
from block 5 and block 4's leave procedure. A hung renderer/write must reach a
native Cancel close / Close anyway decision without premature shutdown.

## Prerequisites and reading

Complete blocks 1–5. Require block 1 approval for page decisions and the native
timeout dialog. Read plan steps 3 and 9; design section 12 and sections 10–11.1;
PRD sections 4.7 and scenarios 1, 8, 11, and 13. Apply Windows work now only in
Windows-available mode. In deferred mode, hand it to conditional block W.

## Main code paths

Use `packaging/macos/main.swift` and `native_bridge.swift`,
`packaging/windows/main.cpp` on Windows, native frontend helpers, and App's
leave/close handling. Extend existing native harnesses where they exercise the
actual path. The macOS harness does not compile production `main.swift`.

## Implementation sequence

### 1. Run the macOS experiment before selecting code

In a local packaged experiment, let `applicationShouldTerminate` return
`.terminateLater` while the runtime and WebKit bridge remain attached. Require
operator evidence for page-to-host messages, host-to-page `callAsyncJavaScript`,
a complete real page/core RPC reply, response-timer delivery with no close
response, and restored interaction after cancellation. Record macOS/WebKit
versions and timer run-loop mode. A development browser does not prove this.

If it passes, use `.terminateLater` for negotiation and the tested timer mode.
Cancel replies false to AppKit; permitted shutdown finishes asynchronously and
replies true. If it fails, use the specified `.terminateCancel` path, negotiate
in the normal run loop, then use a one-use flag to re-enter termination after
permission. Consume the flag to skip only the handshake; retain existing
asynchronous shutdown. Record the fallback's logout/restart limitation.
Keep one selected path, with no runtime fallback detector.

### 2. Add the trusted shell protocol

Use shell messages outside the ordinary RPC queue:

| Direction | Message |
| --- | --- |
| Host → page | `{type: 'prepare-close', attempt_id}` |
| Page → host | `{native_action: 'close_reply', connection_id, attempt_id, result: 'ready' \| 'cancelled'}` |
| Host → page | `{type: 'close-aborted', attempt_id}` |
| Page → host | `{native_action: 'close', connection_id}` |

Use monotonic host-window attempt IDs and current connection checks. Preserve
trusted-frame/origin checks; ignore stale/untrusted responses. Pass values as
JavaScript-call arguments, not interpolated user text.
Install an injected default ready responder during startup; replace it with
App's responder before any editor can exist. App replies ready immediately
only with no editor and no pending navigation, deletion, or save.

### 3. Negotiate before starting shutdown

If runtime or page connection is absent, use direct existing shutdown: no
prepare-close, response timer, or timeout dialog. A connected page with a
failed script call or missing response is different: retain the bounded wait
and native fallback.

Normal close keeps the window, runtime, and bridge alive. Start one attempt
and **15-second** response wait. Repeated close requests do not reset it.
Ready permits shutdown; cancelled ends the wait. Timeout invalidates the
attempt, notifies the page, stops the timer, and shows the approved native
choice. No timer runs while the user decides. Cancel close retains CHA;
Close anyway permits shutdown with the stated unsaved-work warning.

Only after ready, Close anyway, or the applicable direct/fatal path may the
shell set shutdown flags, detach the bridge, set `Closing CHA…`, and start the
existing **ten-second runtime shutdown** deadline. Keep the previous title
through negotiation, page/native decisions, and cancellation.

### 4. Coordinate page flushes and earlier operations

This block implements the complete page-side close handler in App. It uses
block 4's operation slot, release point, decision state, and shared final
flush. Add one nullable `deferredCloseAttemptId`. On prepare-close:

1. If Retry / Stay / Discard is open, reply cancelled at once, retain that
   decision and its original intent, and clear the matching deferred ID.
2. If leave/deletion owns the pending-operation slot, store the attempt ID
   without replying or starting another flush. Its host timer continues.
3. Otherwise reply ready for no work, or acquire the slot and run final flush.

After an earlier action and required editor disposal/restoration release the
slot, take/clear the deferred ID and process the still-current attempt through
the same handler against the resulting editor. If a leave failure opens a
decision, process the ID then to cancel the wait. Resume the same attempt with
remaining time; add no second timer or new close request.

A close flush succeeds by replying ready and staying frozen for shutdown.
Failure replies cancelled immediately, then presents Retry / Stay / Discard.
Stay restores editing with the persistent error. Retry must send a **fresh
close request before saving**: the next prepare-close runs the retry under a
new native timer. Discard settles writes and disposes the editor before a fresh
close request; no-work prepare-close then replies ready. Hold the appropriate
freeze during these handoffs and do not restart a canceled close automatically.

On close-aborted, clear only a matching deferred ID. If close was deferred,
leave the earlier navigation/deletion and its freeze intact. If close owns a
flush, invalidate that continuation and use block 4's aborted-leave cleanup.
An admitted write may finish, but may not reply ready or request another close.
Clear deferred state on page/connection replacement; old aborts must not clear
new attempts.

### 5. Route actual platform entry points

On macOS, keep the existing fatal `runtime != nil, !quitting` guard first.
Use a window delegate so last-window close requests `NSApp.terminate` and
returns false. Quit and page close share the selected termination path; the
window remains visible during decisions.

On Windows, keep Exit, WM_CLOSE, and page close entering `request_close()`.
Keep `closing_` false during negotiation; call `begin_shutdown()` only after
permission. Fatal-error and smoke-test shutdown bypass negotiation directly.
Use native window timer/message handling and a native timeout dialog.

## Automated verification

Test typed shell messages, connection/attempt rejection, initial responder,
no-work behavior, close-owned flushes, deferred attempts, decision cancellation,
Retry ordering, duplicate requests, and aborted late continuations. Extend
Windows harness cases and ensure runner test-name filters execute them. Keep
macOS bridge coverage separate from production termination coverage.
Run relevant frontend, bridge/runtime/native checks, and platform builds as
specified in plan section 5. Harness results do not certify AppKit close.

## Operator checkpoints

First complete and record the packaged termination experiment. Then build the
selected production close path and provide revision, OS/WebKit/WebView2 version,
the block 1 failure-induction method for each case, and numbered expected
outcomes. Use an actual open note in a disposable vault. Run each entry point
(Quit/last-window close or Exit/window close) through:

- Clean/no-work close, and close during startup (the default responder replies
  ready at once).
- No-runtime and no-page-connection direct paths.
- Unsaved edits and an already-pending save; reopen to confirm committed work.
- Save failure followed by Retry, Stay, and Discard. Make Retry hang and verify
  its fresh timer reaches both native choices.
- Close during navigation flush: completion resumes the same timed attempt;
  failure cancels it into the page decision; an already-open decision remains.
- Hung navigation flush and hung/unresponsive connected page: bounded native
  fallback, cancellation, and permitted Close anyway.
- Repeated requests, stale replies, title timing, save completion after cancel,
  and fatal shutdown. Late work must not close the restored app.

Record individual operator results. Add deletion-close cases in block 7 after
deletion exists; they cannot be certified here.

## Completion and handoff

The selected macOS experiment and production save/close checks pass, and Windows
checks pass when in available mode. Record the selected strategy, any fallback
limitation, timers, attempt/abort rules, failure reproduction, build versions,
and pending Windows work. Block 7 uses the same operation slot and deferred ID
for deletion; do not introduce another close coordinator.

## Execution and reporting rules

Apply the split's [execution rules](notes-split.md#execution-rules), including
its operator checkpoint, Windows mode, and end-of-block rules, and its
[handoff contents](notes-split.md#handoff-between-blocks). Apply the scope
rules in plan section 1 and the KISS rules in the repository instructions.

COMPLETED
