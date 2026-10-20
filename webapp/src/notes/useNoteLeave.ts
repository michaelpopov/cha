import { useCallback, useRef, useState } from 'react';

import { replyNativeClose, requestNativeClose } from '../api/nativeClose';
import { restoreAppUrl } from '../state/route';
import type { NoteSaveHandle } from './useNoteSave';

export type LeaveNoteThen = <T>(
  action: () => T | Promise<T>,
  options?: { previousUrl?: string },
) => Promise<T | false>;

export type PendingOperationKind = 'leave' | 'action' | null;

type LeaveChoice = 'retry' | 'stay' | 'discard';
type IntentKind = 'navigate' | 'close';
type ClosePhase = 'flush' | 'retry-handoff' | 'discard-handoff';

type LeaveIntent = {
  kind: IntentKind;
  action: () => unknown;
  previousUrl?: string;
  canceledUncovered: boolean;
  submitted: boolean;
  choose?: (choice: LeaveChoice) => void;
  handoff?: (attemptId: number | null) => void;
  retryWait?: Promise<number | null>;
};

function noCloseWork(save: NoteSaveHandle | null): boolean {
  if (!save) return true;
  return !save.isOpen && save.inFlightSerial() === null && !save.hasUnsaved();
}

export function useNoteLeave(
  getSave: () => NoteSaveHandle | null,
  options?: { disposeEditor?: () => void },
) {
  const getSaveRef = useRef(getSave);
  getSaveRef.current = getSave;
  const disposeEditorRef = useRef(options?.disposeEditor);
  disposeEditorRef.current = options?.disposeEditor;
  const intentRef = useRef<LeaveIntent | null>(null);
  const runningAction = useRef(false);
  const pendingKindRef = useRef<PendingOperationKind>(null);
  const decisionOpenRef = useRef(false);
  const deferredCloseAttemptId = useRef<number | null>(null);
  const closePhase = useRef<ClosePhase | null>(null);
  const closeAttempt = useRef<number | null>(null);
  const retiredCloseAttempt = useRef(0);
  const closeToken = useRef(0);
  const prepareCloseRef = useRef<(attemptId: number) => void>(() => undefined);
  const [pendingKind, setPendingKind] = useState<PendingOperationKind>(null);
  const [decisionOpen, setDecisionOpen] = useState(false);

  const setSlot = useCallback((kind: PendingOperationKind) => {
    pendingKindRef.current = kind;
    setPendingKind(kind);
  }, []);

  const setDecision = useCallback((open: boolean) => {
    decisionOpenRef.current = open;
    setDecisionOpen(open);
  }, []);

  const resumeDeferredClose = useCallback(() => {
    const attemptId = deferredCloseAttemptId.current;
    if (attemptId === null) return;
    deferredCloseAttemptId.current = null;
    prepareCloseRef.current(attemptId);
  }, []);

  const release = useCallback((intent: LeaveIntent) => {
    if (intentRef.current !== intent) return;
    intentRef.current = null;
    intent.choose = undefined;
    intent.handoff = undefined;
    closePhase.current = null;
    closeAttempt.current = null;
    setDecision(false);
    setSlot(null);
    resumeDeferredClose();
  }, [resumeDeferredClose, setDecision, setSlot]);

  const invokeAction = useCallback(<T>(action: () => T | Promise<T>): T | Promise<T> => {
    runningAction.current = true;
    try {
      return action();
    } finally {
      runningAction.current = false;
    }
  }, []);

  const finish = useCallback(async (
    intent: LeaveIntent,
    save: NoteSaveHandle,
    mode: 'keep' | 'discard',
  ): Promise<unknown> => {
    if (intentRef.current !== intent) return false;
    if (mode === 'discard') {
      await save.waitInFlight();
      if (intentRef.current !== intent) return false;
      save.dispose();
      disposeEditorRef.current?.();
    }
    let result: unknown = false;
    let thrown: unknown;
    let failed = false;
    try {
      result = await invokeAction(intent.action);
      if (result === false) failed = true;
    } catch (error) {
      thrown = error;
      failed = true;
    }
    if (intentRef.current !== intent) {
      if (thrown) throw thrown;
      return false;
    }
    if (mode === 'keep' && failed) save.unfreeze();
    release(intent);
    if (thrown) throw thrown;
    return failed ? false : result;
  }, [invokeAction, release]);

  const waitForChoice = useCallback((intent: LeaveIntent) => {
    setDecision(true);
    const deferred = deferredCloseAttemptId.current;
    if (deferred !== null) {
      deferredCloseAttemptId.current = null;
      prepareCloseRef.current(deferred);
    }
    return new Promise<LeaveChoice>((resolve) => {
      intent.choose = resolve;
    });
  }, [setDecision]);

  const stayIntent = useCallback((intent: LeaveIntent) => {
    if (intent.previousUrl !== undefined) restoreAppUrl(intent.previousUrl);
    const save = getSaveRef.current();
    save?.unfreeze();
    if (intent.canceledUncovered && !intent.submitted) save?.submitNow();
    release(intent);
  }, [release]);

  const resolveLeave = useCallback(async (
    intent: LeaveIntent,
    save: NoteSaveHandle,
  ): Promise<unknown> => {
    while (intentRef.current === intent) {
      const admitted = save.inFlightSerial();
      await save.waitInFlight();
      if (intentRef.current !== intent) return false;
      const current = getSaveRef.current() ?? save;
      if (admitted !== null && current.saveError?.serial === admitted) {
        const choice = await waitForChoice(intent);
        if (choice === 'retry') continue;
        if (choice === 'stay') {
          stayIntent(intent);
          return false;
        }
        return finish(intent, save, 'discard');
      }
      if (save.hasUnsaved()) {
        // This flush submits the changes whose deadline the freeze canceled.
        intent.canceledUncovered = false;
        intent.submitted = true;
        const ok = await save.flushRemaining();
        if (intentRef.current !== intent) return false;
        if (!ok) {
          const choice = await waitForChoice(intent);
          if (choice === 'retry') continue;
          if (choice === 'stay') {
            stayIntent(intent);
            return false;
          }
          return finish(intent, save, 'discard');
        }
      }
      return finish(intent, save, 'keep');
    }
    return false;
  }, [finish, stayIntent, waitForChoice]);

  const beginNoteAction = useCallback(() => {
    if (intentRef.current || pendingKindRef.current !== null || runningAction.current) return false;
    setSlot('action');
    return true;
  }, [setSlot]);

  const endNoteAction = useCallback(() => {
    if (pendingKindRef.current !== 'action') return;
    setSlot(null);
    resumeDeferredClose();
  }, [resumeDeferredClose, setSlot]);

  const leaveNoteThen = useCallback<LeaveNoteThen>(async (action, leaveOptions) => {
    if (runningAction.current) return await action();
    if (intentRef.current || pendingKindRef.current !== null) {
      if (leaveOptions?.previousUrl !== undefined) restoreAppUrl(leaveOptions.previousUrl);
      return false;
    }
    const save = getSaveRef.current();
    if (!save?.isOpen) return await invokeAction(action);
    const intent: LeaveIntent = {
      kind: 'navigate',
      action,
      previousUrl: leaveOptions?.previousUrl,
      canceledUncovered: false,
      submitted: false,
    };
    intentRef.current = intent;
    setSlot('leave');
    intent.canceledUncovered = save.freeze();
    try {
      return await resolveLeave(intent, save) as Awaited<ReturnType<typeof action>> | false;
    } catch (error) {
      if (intentRef.current === intent) {
        save.unfreeze();
        release(intent);
      }
      throw error;
    }
  }, [invokeAction, release, resolveLeave, setSlot]);

  const currentChoice = useCallback(() => {
    const intent = intentRef.current;
    if (!intent?.choose) return null;
    const choose = intent.choose;
    intent.choose = undefined;
    setDecision(false);
    return { intent, choose };
  }, [setDecision]);

  const retry = useCallback(() => {
    const pending = currentChoice();
    if (!pending) return;
    if (pending.intent.kind === 'close') {
      closePhase.current = 'retry-handoff';
      pending.intent.retryWait = new Promise((resolve) => {
        pending.intent.handoff = resolve;
      });
      requestNativeClose();
    }
    pending.choose('retry');
  }, [currentChoice]);

  const stay = useCallback(() => {
    const pending = currentChoice();
    pending?.choose('stay');
  }, [currentChoice]);

  const discard = useCallback(() => {
    const pending = currentChoice();
    pending?.choose('discard');
  }, [currentChoice]);

  const closeStillOwned = useCallback((intent: LeaveIntent, token: number) => (
    intentRef.current === intent && closeToken.current === token
  ), []);

  const runClose = useCallback(async (
    intent: LeaveIntent,
    save: NoteSaveHandle,
    token: number,
  ) => {
    let attemptId = closeAttempt.current;
    while (closeStillOwned(intent, token) && attemptId !== null) {
      const admitted = save.inFlightSerial();
      await save.waitInFlight();
      if (!closeStillOwned(intent, token) || closeAttempt.current !== attemptId) return;
      const current = getSaveRef.current() ?? save;
      const failedAdmitted = admitted !== null && current.saveError?.serial === admitted;
      const needsFlush = !failedAdmitted && current.hasUnsaved();
      if (needsFlush) {
        intent.canceledUncovered = false;
        intent.submitted = true;
        const ok = await current.flushRemaining();
        if (!closeStillOwned(intent, token) || closeAttempt.current !== attemptId) return;
        if (ok) {
          replyNativeClose(attemptId, 'ready');
          closePhase.current = null;
          return;
        }
      } else if (!failedAdmitted) {
        replyNativeClose(attemptId, 'ready');
        closePhase.current = null;
        return;
      }
      replyNativeClose(attemptId, 'cancelled');
      if (closeAttempt.current === attemptId) closeAttempt.current = null;
      retiredCloseAttempt.current = attemptId;
      const choice = await waitForChoice(intent);
      if (!closeStillOwned(intent, token)) return;
      if (choice === 'stay') {
        stayIntent(intent);
        return;
      }
      if (choice === 'discard') {
        await save.waitInFlight();
        if (!closeStillOwned(intent, token)) return;
        save.dispose();
        disposeEditorRef.current?.();
        closePhase.current = 'discard-handoff';
        closeAttempt.current = null;
        requestNativeClose();
        return;
      }
      const next = await intent.retryWait;
      intent.retryWait = undefined;
      if (next == null || !closeStillOwned(intent, token)) return;
      attemptId = next;
    }
  }, [closeStillOwned, stayIntent, waitForChoice]);

  const startClose = useCallback((attemptId: number) => {
    const save = getSaveRef.current();
    if (noCloseWork(save)) {
      replyNativeClose(attemptId, 'ready');
      return;
    }
    const intent: LeaveIntent = {
      kind: 'close',
      action: () => undefined,
      canceledUncovered: false,
      submitted: false,
    };
    intentRef.current = intent;
    setSlot('leave');
    intent.canceledUncovered = save?.freeze() ?? false;
    closePhase.current = 'flush';
    closeAttempt.current = attemptId;
    const token = closeToken.current;
    void runClose(intent, save as NoteSaveHandle, token);
  }, [runClose, setSlot]);

  const prepareClose = useCallback((attemptId: number) => {
    // A paused renderer can receive the abort before the queued prepare call.
    if (attemptId <= retiredCloseAttempt.current) return;
    if (decisionOpenRef.current) {
      if (deferredCloseAttemptId.current === attemptId) {
        deferredCloseAttemptId.current = null;
      }
      replyNativeClose(attemptId, 'cancelled');
      return;
    }
    const intent = intentRef.current;
    if (intent?.kind === 'close' && closePhase.current === 'retry-handoff') {
      closePhase.current = 'flush';
      closeAttempt.current = attemptId;
      const handoff = intent.handoff;
      intent.handoff = undefined;
      handoff?.(attemptId);
      return;
    }
    if (intent?.kind === 'close' && closePhase.current === 'discard-handoff') {
      closePhase.current = null;
      closeAttempt.current = attemptId;
      replyNativeClose(attemptId, 'ready');
      return;
    }
    if (pendingKindRef.current !== null) {
      deferredCloseAttemptId.current = attemptId;
      return;
    }
    startClose(attemptId);
  }, [startClose]);
  prepareCloseRef.current = prepareClose;

  const abortOwnedClose = useCallback((intent: LeaveIntent) => {
    closeToken.current += 1;
    closePhase.current = null;
    closeAttempt.current = null;
    const handoff = intent.handoff;
    intent.handoff = undefined;
    handoff?.(null);
    if (intentRef.current !== intent) return;
    const save = getSaveRef.current();
    save?.unfreeze();
    if (intent.canceledUncovered && !intent.submitted) save?.submitNow();
    intentRef.current = null;
    intent.choose = undefined;
    setDecision(false);
    setSlot(null);
  }, [setDecision, setSlot]);

  const closeAborted = useCallback((attemptId: number) => {
    const newerAttempt = attemptId > retiredCloseAttempt.current;
    retiredCloseAttempt.current = Math.max(retiredCloseAttempt.current, attemptId);
    if (deferredCloseAttemptId.current === attemptId) {
      deferredCloseAttemptId.current = null;
    }
    const intent = intentRef.current;
    if (intent?.kind !== 'close') return;
    if (closeAttempt.current === attemptId) {
      abortOwnedClose(intent);
      return;
    }
    const waitingForNextAttempt = closePhase.current === 'retry-handoff'
      || closePhase.current === 'discard-handoff';
    if (waitingForNextAttempt && newerAttempt) {
      abortOwnedClose(intent);
    }
  }, [abortOwnedClose]);

  return {
    leaveNoteThen,
    beginNoteAction,
    endNoteAction,
    retry,
    stay,
    discard,
    prepareClose,
    closeAborted,
    decisionOpen,
    pendingKind,
    holdsLeave: pendingKind === 'leave',
  };
}
