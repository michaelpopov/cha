import { useCallback, useRef, useState } from 'react';

import { restoreAppUrl } from '../state/route';
import type { NoteSaveHandle } from './useNoteSave';

export type LeaveNoteThen = <T>(
  action: () => T | Promise<T>,
  options?: { previousUrl?: string },
) => Promise<T | false>;

export type PendingOperationKind = 'leave' | 'delete' | null;

type LeaveChoice = 'retry' | 'stay' | 'discard';

type LeaveIntent = {
  action: () => unknown;
  previousUrl?: string;
  canceledUncovered: boolean;
  choose?: (choice: LeaveChoice) => void;
};

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
  const [pendingKind, setPendingKind] = useState<PendingOperationKind>(null);
  const [decisionOpen, setDecisionOpen] = useState(false);

  const release = useCallback((intent: LeaveIntent) => {
    if (intentRef.current !== intent) return;
    intentRef.current = null;
    intent.choose = undefined;
    setDecisionOpen(false);
    setPendingKind(null);
  }, []);

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
    setDecisionOpen(true);
    return new Promise<LeaveChoice>((resolve) => {
      intent.choose = resolve;
    });
  }, []);

  const stayIntent = useCallback((intent: LeaveIntent) => {
    if (intent.previousUrl !== undefined) restoreAppUrl(intent.previousUrl);
    const save = getSaveRef.current();
    save?.unfreeze();
    if (intent.canceledUncovered) save?.submitNow();
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

  const leaveNoteThen = useCallback<LeaveNoteThen>(async (action, leaveOptions) => {
    if (runningAction.current) return await action();
    if (intentRef.current) {
      if (leaveOptions?.previousUrl !== undefined) restoreAppUrl(leaveOptions.previousUrl);
      return false;
    }
    const save = getSaveRef.current();
    if (!save?.isOpen) return await invokeAction(action);
    const intent: LeaveIntent = {
      action,
      previousUrl: leaveOptions?.previousUrl,
      canceledUncovered: false,
    };
    intentRef.current = intent;
    setPendingKind('leave');
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
  }, [invokeAction, release, resolveLeave]);

  const retry = useCallback(() => {
    const intent = intentRef.current;
    if (!intent?.choose) return;
    const choose = intent.choose;
    intent.choose = undefined;
    setDecisionOpen(false);
    choose('retry');
  }, []);

  const stay = useCallback(() => {
    const intent = intentRef.current;
    if (!intent?.choose) return;
    const choose = intent.choose;
    intent.choose = undefined;
    setDecisionOpen(false);
    choose('stay');
  }, []);

  const discard = useCallback(() => {
    const intent = intentRef.current;
    if (!intent?.choose) return;
    const choose = intent.choose;
    intent.choose = undefined;
    setDecisionOpen(false);
    choose('discard');
  }, []);

  return {
    leaveNoteThen,
    retry,
    stay,
    discard,
    decisionOpen,
    pendingKind,
    holdsLeave: pendingKind === 'leave',
  };
}
