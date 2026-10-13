import type { Editor } from '@tiptap/core';
import { act, renderHook } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { ChaError } from '../api/client';
import { rememberAppUrl, rememberedAppUrl, restoreAppUrl } from '../state/route';
import { createNotesEditor } from './editor';
import { useNoteLeave } from './useNoteLeave';
import { noteSaveDelayMs, useNoteSave, type NoteSaveHandle } from './useNoteSave';

function deferred<T = void>() {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((ok, err) => {
    resolve = ok;
    reject = err;
  });
  return { promise, resolve, reject };
}

function openSave(overrides: Partial<NoteSaveHandle> = {}): NoteSaveHandle {
  return {
    isOpen: true,
    frozen: false,
    saveError: null,
    hasUnsaved: () => true,
    inFlightSerial: () => null,
    waitInFlight: async () => undefined,
    freeze: vi.fn(() => false),
    unfreeze: vi.fn(),
    submitNow: vi.fn(),
    flushRemaining: vi.fn(async () => true),
    dispose: vi.fn(),
    ...overrides,
  };
}

afterEach(() => {
  window.history.replaceState(null, '', '/');
  rememberAppUrl();
});

describe('leaveNoteThen', () => {
  it('runs the action immediately when no note is open', async () => {
    const view = renderHook(() => useNoteLeave(() => null));
    const action = vi.fn(() => 'ok');
    await expect(view.result.current.leaveNoteThen(action)).resolves.toBe('ok');
    expect(action).toHaveBeenCalledOnce();
    expect(view.result.current.pendingKind).toBeNull();
  });

  it('flushes then runs the action and releases the slot', async () => {
    const save = openSave();
    const disposeEditor = vi.fn();
    const view = renderHook(() => useNoteLeave(() => save, { disposeEditor }));
    const action = vi.fn(() => 'done');
    await act(async () => {
      await expect(view.result.current.leaveNoteThen(action)).resolves.toBe('done');
    });
    expect(save.freeze).toHaveBeenCalledOnce();
    expect(save.flushRemaining).toHaveBeenCalledOnce();
    expect(action).toHaveBeenCalledOnce();
    expect(disposeEditor).not.toHaveBeenCalled();
    expect(view.result.current.pendingKind).toBeNull();
    expect(view.result.current.decisionOpen).toBe(false);
    expect(view.result.current.holdsLeave).toBe(false);
  });

  it('opens the decision when the settled handle reports the admitted failure', async () => {
    const admitted = deferred();
    const flushRemaining = vi.fn(async () => true);
    let current: NoteSaveHandle;
    const save = openSave({
      inFlightSerial: () => 4,
      saveError: null,
      flushRemaining,
      waitInFlight: () => admitted.promise.then(() => {
        current = {
          ...save,
          saveError: { serial: 4, message: 'The note could not be saved.' },
        };
      }),
    });
    current = save;
    const view = renderHook(() => useNoteLeave(() => current));
    const action = vi.fn();
    let finished: Promise<unknown> | undefined;
    act(() => {
      finished = view.result.current.leaveNoteThen(action);
    });
    await act(async () => {
      await Promise.resolve();
    });
    expect(view.result.current.decisionOpen).toBe(false);
    await act(async () => {
      admitted.resolve();
      await admitted.promise;
    });
    expect(view.result.current.decisionOpen).toBe(true);
    expect(flushRemaining).not.toHaveBeenCalled();
    expect(action).not.toHaveBeenCalled();
    await act(async () => {
      view.result.current.stay();
      await finished;
    });
    expect(view.result.current.pendingKind).toBeNull();
  });

  it('opens the decision when an admitted save fails before the next handle render', async () => {
    vi.useFakeTimers();
    const editors: Editor[] = [];
    try {
      const first = deferred();
      const saveNote = vi.fn(() => first.promise);
      const editor = createNotesEditor();
      const element = editor.options.element;
      if (element instanceof HTMLElement && !element.isConnected) document.body.appendChild(element);
      editors.push(editor);
      const saveRef: { current: NoteSaveHandle | null } = { current: null };
      renderHook(() => {
        const handle = useNoteSave({
          editor,
          noteId: 4,
          contextKey: 'vault',
          save: saveNote,
        });
        saveRef.current = handle;
        return handle;
      });
      editor.view.dispatch(editor.state.tr.insertText('a'));
      await act(async () => {
        await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
      });
      expect(saveNote).toHaveBeenCalledOnce();
      const view = renderHook(() => useNoteLeave(() => saveRef.current));
      const action = vi.fn();
      let finished: Promise<unknown> | undefined;
      act(() => {
        finished = view.result.current.leaveNoteThen(action);
      });
      await act(async () => {
        await Promise.resolve();
      });
      expect(view.result.current.decisionOpen).toBe(false);
      await act(async () => {
        first.reject(new ChaError('internal_error', 'The note could not be saved.'));
        await first.promise.catch(() => undefined);
      });
      expect(view.result.current.decisionOpen).toBe(true);
      expect(saveNote).toHaveBeenCalledOnce();
      expect(action).not.toHaveBeenCalled();
      await act(async () => {
        view.result.current.stay();
        await finished;
      });
      expect(view.result.current.pendingKind).toBeNull();
    } finally {
      for (const editor of editors) editor.destroy();
      document.body.replaceChildren();
      vi.useRealTimers();
    }
  });

  it('opens the decision when an admitted write fails during the flush', async () => {
    const admitted = deferred();
    const save = openSave({
      inFlightSerial: () => 3,
      waitInFlight: () => admitted.promise,
      saveError: { serial: 3, message: 'The note could not be saved.' },
      flushRemaining: vi.fn(async () => true),
    });
    const view = renderHook(() => useNoteLeave(() => save));
    const action = vi.fn();
    let finished: Promise<unknown> | undefined;
    act(() => {
      finished = view.result.current.leaveNoteThen(action);
    });
    await act(async () => {
      await Promise.resolve();
    });
    expect(view.result.current.holdsLeave).toBe(true);
    expect(view.result.current.decisionOpen).toBe(false);
    await act(async () => {
      admitted.resolve();
      await admitted.promise;
    });
    expect(view.result.current.decisionOpen).toBe(true);
    expect(save.flushRemaining).not.toHaveBeenCalled();
    expect(action).not.toHaveBeenCalled();
    await act(async () => {
      view.result.current.stay();
      await finished;
    });
    expect(view.result.current.decisionOpen).toBe(false);
    expect(view.result.current.pendingKind).toBeNull();
  });

  it('retries a failed remaining flush and then runs the action', async () => {
    const save = openSave({
      flushRemaining: vi.fn()
        .mockResolvedValueOnce(false)
        .mockResolvedValueOnce(true),
    });
    const view = renderHook(() => useNoteLeave(() => save));
    const action = vi.fn(() => true);
    let finished: Promise<unknown> | undefined;
    act(() => {
      finished = view.result.current.leaveNoteThen(action);
    });
    await act(async () => {
      await Promise.resolve();
    });
    expect(view.result.current.decisionOpen).toBe(true);
    expect(action).not.toHaveBeenCalled();
    await act(async () => {
      view.result.current.retry();
      await finished;
    });
    expect(save.flushRemaining).toHaveBeenCalledTimes(2);
    expect(action).toHaveBeenCalledOnce();
    expect(view.result.current.pendingKind).toBeNull();
  });

  it('restores the captured URL on Stay without resending a failed final flush', async () => {
    const save = openSave({
      freeze: vi.fn(() => true),
      flushRemaining: vi.fn(async () => false),
    });
    const view = renderHook(() => useNoteLeave(() => save));
    window.history.replaceState(null, '', '/#/s/lobby/planning/');
    rememberAppUrl();
    const previousUrl = rememberedAppUrl();
    window.history.replaceState(null, '', '/#/');
    rememberAppUrl();
    const action = vi.fn();
    let finished: Promise<unknown> | undefined;
    act(() => {
      finished = view.result.current.leaveNoteThen(action, { previousUrl });
    });
    await act(async () => {
      await Promise.resolve();
    });
    await act(async () => {
      view.result.current.stay();
      await finished;
    });
    expect(action).not.toHaveBeenCalled();
    expect(save.unfreeze).toHaveBeenCalled();
    expect(save.flushRemaining).toHaveBeenCalledOnce();
    expect(save.submitNow).not.toHaveBeenCalled();
    expect(window.location.hash).toBe('#/s/lobby/planning/');
    expect(rememberedAppUrl()).toBe(window.location.href);
    expect(view.result.current.pendingKind).toBeNull();
  });

  it('submits canceled uncovered changes on Stay after an admitted save fails', async () => {
    const save = openSave({
      freeze: vi.fn(() => true),
      inFlightSerial: () => 3,
      saveError: { serial: 3, message: 'The note could not be saved.' },
    });
    const view = renderHook(() => useNoteLeave(() => save));
    const action = vi.fn();
    let finished: Promise<unknown> | undefined;
    act(() => {
      finished = view.result.current.leaveNoteThen(action);
    });
    await act(async () => {
      await Promise.resolve();
    });
    expect(view.result.current.decisionOpen).toBe(true);
    await act(async () => {
      view.result.current.stay();
      await finished;
    });
    expect(action).not.toHaveBeenCalled();
    expect(save.flushRemaining).not.toHaveBeenCalled();
    expect(save.submitNow).toHaveBeenCalledOnce();
  });

  it('discards after settling, disposes the editor, then runs the action', async () => {
    const save = openSave({
      flushRemaining: vi.fn(async () => false),
    });
    const disposeEditor = vi.fn();
    const view = renderHook(() => useNoteLeave(() => save, { disposeEditor }));
    const action = vi.fn(() => 'gone');
    let finished: Promise<unknown> | undefined;
    act(() => {
      finished = view.result.current.leaveNoteThen(action);
    });
    await act(async () => {
      await Promise.resolve();
    });
    await act(async () => {
      view.result.current.discard();
      await finished;
    });
    expect(save.dispose).toHaveBeenCalled();
    expect(disposeEditor).toHaveBeenCalled();
    expect(action).toHaveBeenCalledOnce();
    expect(save.unfreeze).not.toHaveBeenCalled();
    expect(view.result.current.pendingKind).toBeNull();
  });

  it('restores editing when the destination fails after a successful save', async () => {
    const save = openSave();
    const view = renderHook(() => useNoteLeave(() => save));
    await act(async () => {
      await expect(view.result.current.leaveNoteThen(() => false)).resolves.toBe(false);
    });
    expect(save.unfreeze).toHaveBeenCalled();
    expect(view.result.current.pendingKind).toBeNull();
  });

  it('keeps the editor closed when the destination fails after Discard', async () => {
    const save = openSave({
      flushRemaining: vi.fn(async () => false),
    });
    const disposeEditor = vi.fn();
    const view = renderHook(() => useNoteLeave(() => save, { disposeEditor }));
    let finished: Promise<unknown> | undefined;
    act(() => {
      finished = view.result.current.leaveNoteThen(() => false);
    });
    await act(async () => {
      await Promise.resolve();
    });
    await act(async () => {
      view.result.current.discard();
      await finished;
    });
    expect(disposeEditor).toHaveBeenCalled();
    expect(save.unfreeze).not.toHaveBeenCalled();
  });

  it('ignores a second navigation and restores that request URL', async () => {
    const hang = deferred();
    const save = openSave({
      waitInFlight: () => hang.promise,
      inFlightSerial: () => 1,
      saveError: null,
    });
    const view = renderHook(() => useNoteLeave(() => save));
    const first = vi.fn();
    const second = vi.fn();
    act(() => {
      void view.result.current.leaveNoteThen(first);
    });
    window.history.replaceState(null, '', '/#/s/lobby/planning/');
    rememberAppUrl();
    const previousUrl = rememberedAppUrl();
    window.history.replaceState(null, '', '/#/');
    rememberAppUrl();
    await act(async () => {
      await expect(view.result.current.leaveNoteThen(second, { previousUrl })).resolves.toBe(false);
    });
    expect(second).not.toHaveBeenCalled();
    expect(window.location.hash).toBe('#/s/lobby/planning/');
    await act(async () => {
      hang.resolve();
      await hang.promise;
    });
    expect(first).toHaveBeenCalledOnce();
  });

  it('ignores stale Retry after Stay', async () => {
    const save = openSave({
      flushRemaining: vi.fn(async () => false),
    });
    const view = renderHook(() => useNoteLeave(() => save));
    let finished: Promise<unknown> | undefined;
    act(() => {
      finished = view.result.current.leaveNoteThen(() => 'go');
    });
    await act(async () => {
      await Promise.resolve();
    });
    await act(async () => {
      view.result.current.stay();
      await finished;
    });
    act(() => {
      view.result.current.retry();
      view.result.current.discard();
    });
    expect(save.flushRemaining).toHaveBeenCalledOnce();
    expect(view.result.current.pendingKind).toBeNull();
  });

  it('does not run a late action after Stay', async () => {
    const hang = deferred();
    const save = openSave({
      waitInFlight: () => hang.promise,
      inFlightSerial: () => 2,
      saveError: { serial: 2, message: 'The note could not be saved.' },
    });
    const view = renderHook(() => useNoteLeave(() => save));
    const action = vi.fn();
    let finished: Promise<unknown> | undefined;
    act(() => {
      finished = view.result.current.leaveNoteThen(action);
    });
    await act(async () => {
      await Promise.resolve();
    });
    await act(async () => {
      hang.resolve();
      await hang.promise;
    });
    act(() => {
      view.result.current.stay();
    });
    await expect(finished).resolves.toBe(false);
    expect(action).not.toHaveBeenCalled();
  });
});

function closePosts(): Array<Record<string, unknown>> {
  const post = window.__CHA_NATIVE_POST__;
  if (!post) return [];
  return (post as unknown as { mock: { calls: string[][] } }).mock.calls.map(
    (call) => JSON.parse(call[0]) as Record<string, unknown>,
  );
}

describe('native close', () => {
  afterEach(() => {
    delete window.__CHA_NATIVE_POST__;
    delete window.__CHA_NATIVE_CONNECTION_ID__;
  });

  function host() {
    window.__CHA_NATIVE_CONNECTION_ID__ = 'view-4';
    window.__CHA_NATIVE_POST__ = vi.fn();
  }

  it('replies ready immediately when there is no editor and no save', () => {
    host();
    const view = renderHook(() => useNoteLeave(() => null));
    act(() => {
      view.result.current.prepareClose(1);
    });
    expect(closePosts()).toEqual([{
      native_action: 'close_reply',
      connection_id: 'view-4',
      attempt_id: 1,
      result: 'ready',
    }]);
    expect(view.result.current.pendingKind).toBeNull();
  });

  it('replies ready for an open note with no unsaved work and does not freeze it', () => {
    host();
    const save = openSave({
      hasUnsaved: () => false,
      inFlightSerial: () => null,
    });
    const view = renderHook(() => useNoteLeave(() => save));
    act(() => {
      view.result.current.prepareClose(2);
    });
    expect(closePosts()[0]?.result).toBe('ready');
    expect(save.freeze).not.toHaveBeenCalled();
  });

  it('flushes a close-owned save, replies ready, and stays frozen', async () => {
    host();
    const save = openSave();
    const view = renderHook(() => useNoteLeave(() => save));
    act(() => {
      view.result.current.prepareClose(3);
    });
    await act(async () => {
      await Promise.resolve();
    });
    expect(save.freeze).toHaveBeenCalledOnce();
    expect(save.flushRemaining).toHaveBeenCalledOnce();
    expect(save.unfreeze).not.toHaveBeenCalled();
    expect(closePosts()).toEqual([{
      native_action: 'close_reply',
      connection_id: 'view-4',
      attempt_id: 3,
      result: 'ready',
    }]);
    expect(view.result.current.pendingKind).toBe('leave');
  });

  it('replies cancelled before the decision when the close flush fails', async () => {
    host();
    const save = openSave({
      flushRemaining: vi.fn(async () => false),
    });
    const view = renderHook(() => useNoteLeave(() => save));
    act(() => {
      view.result.current.prepareClose(4);
    });
    await act(async () => {
      await Promise.resolve();
    });
    expect(view.result.current.decisionOpen).toBe(true);
    expect(closePosts()).toEqual([{
      native_action: 'close_reply',
      connection_id: 'view-4',
      attempt_id: 4,
      result: 'cancelled',
    }]);
    expect(save.unfreeze).not.toHaveBeenCalled();
  });

  it('cancels a prepare-close that arrives while the decision is open', async () => {
    host();
    const save = openSave({
      flushRemaining: vi.fn(async () => false),
    });
    const view = renderHook(() => useNoteLeave(() => save));
    act(() => {
      view.result.current.prepareClose(4);
    });
    await act(async () => {
      await Promise.resolve();
    });
    act(() => {
      view.result.current.prepareClose(5);
    });
    expect(save.flushRemaining).toHaveBeenCalledOnce();
    expect(view.result.current.decisionOpen).toBe(true);
    expect(closePosts().map((message) => message.attempt_id)).toEqual([4, 5]);
    expect(closePosts().every((message) => message.result === 'cancelled')).toBe(true);
  });

  it('defers a close behind a leave and resumes that attempt after the slot is free', async () => {
    host();
    const hang = deferred();
    let unsaved = true;
    const save = openSave({
      waitInFlight: () => hang.promise,
      inFlightSerial: () => 1,
      saveError: null,
      hasUnsaved: () => unsaved,
      flushRemaining: vi.fn(async () => {
        unsaved = false;
        return true;
      }),
    });
    const view = renderHook(() => useNoteLeave(() => save));
    const action = vi.fn(() => true);
    act(() => {
      void view.result.current.leaveNoteThen(action);
    });
    act(() => {
      view.result.current.prepareClose(6);
    });
    expect(closePosts()).toEqual([]);
    expect(save.flushRemaining).not.toHaveBeenCalled();
    await act(async () => {
      hang.resolve();
      await hang.promise;
      await new Promise((resolve) => setTimeout(resolve, 0));
    });
    expect(action).toHaveBeenCalledOnce();
    expect(save.flushRemaining).toHaveBeenCalledOnce();
    expect(closePosts()).toEqual([{
      native_action: 'close_reply',
      connection_id: 'view-4',
      attempt_id: 6,
      result: 'ready',
    }]);
  });

  it('cancels the deferred close when a leave failure opens the decision', async () => {
    host();
    const hang = deferred();
    const save = openSave({
      waitInFlight: () => hang.promise,
      inFlightSerial: () => 3,
      saveError: { serial: 3, message: 'The note could not be saved.' },
    });
    const view = renderHook(() => useNoteLeave(() => save));
    act(() => {
      void view.result.current.leaveNoteThen(vi.fn());
    });
    act(() => {
      view.result.current.prepareClose(7);
    });
    await act(async () => {
      hang.resolve();
      await hang.promise;
    });
    expect(view.result.current.decisionOpen).toBe(true);
    expect(closePosts()).toEqual([{
      native_action: 'close_reply',
      connection_id: 'view-4',
      attempt_id: 7,
      result: 'cancelled',
    }]);
    expect(save.flushRemaining).not.toHaveBeenCalled();
  });

  it('sends a fresh close request before the retry flush', async () => {
    host();
    const order: string[] = [];
    window.__CHA_NATIVE_POST__ = vi.fn((message: string) => {
      const parsed = JSON.parse(message) as { native_action?: string };
      if (parsed.native_action === 'close') order.push('close');
    });
    const save = openSave({
      flushRemaining: vi.fn()
        .mockImplementationOnce(async () => {
          order.push('flush');
          return false;
        })
        .mockImplementationOnce(async () => {
          order.push('flush');
          return true;
        }),
    });
    const view = renderHook(() => useNoteLeave(() => save));
    act(() => {
      view.result.current.prepareClose(8);
    });
    await act(async () => {
      await Promise.resolve();
    });
    act(() => {
      view.result.current.retry();
    });
    expect(order).toEqual(['flush', 'close']);
    expect(save.flushRemaining).toHaveBeenCalledOnce();
    act(() => {
      view.result.current.prepareClose(9);
    });
    await act(async () => {
      await Promise.resolve();
    });
    expect(order).toEqual(['flush', 'close', 'flush']);
    expect(closePosts().at(-1)).toMatchObject({
      native_action: 'close_reply',
      attempt_id: 9,
      result: 'ready',
    });
  });

  it('disposes the editor and requests close before the no-work ready reply', async () => {
    host();
    const save = openSave({
      flushRemaining: vi.fn(async () => false),
    });
    const disposeEditor = vi.fn();
    const view = renderHook(() => useNoteLeave(() => save, { disposeEditor }));
    act(() => {
      view.result.current.prepareClose(10);
    });
    await act(async () => {
      await Promise.resolve();
    });
    await act(async () => {
      view.result.current.discard();
      await Promise.resolve();
    });
    expect(save.dispose).toHaveBeenCalledOnce();
    expect(disposeEditor).toHaveBeenCalledOnce();
    expect(closePosts().some((message) => message.native_action === 'close')).toBe(true);
    expect(closePosts().some((message) => message.result === 'ready')).toBe(false);
    act(() => {
      view.result.current.prepareClose(11);
    });
    expect(closePosts().at(-1)).toMatchObject({
      native_action: 'close_reply',
      attempt_id: 11,
      result: 'ready',
    });
    act(() => {
      view.result.current.closeAborted(11);
    });
    expect(view.result.current.pendingKind).toBeNull();
    expect(closePosts().some((message) => message.native_action === 'close')).toBe(true);
    expect(closePosts().filter((message) => message.result === 'ready')).toHaveLength(1);
  });

  it('keeps a retry when an older abort arrives and releases it for a newer abort', async () => {
    host();
    const save = openSave({
      flushRemaining: vi.fn(async () => false),
    });
    const view = renderHook(() => useNoteLeave(() => save));
    act(() => {
      view.result.current.prepareClose(20);
    });
    await act(async () => {
      await Promise.resolve();
    });
    act(() => {
      view.result.current.retry();
      view.result.current.closeAborted(20);
    });
    expect(view.result.current.pendingKind).toBe('leave');
    expect(save.flushRemaining).toHaveBeenCalledOnce();
    act(() => {
      view.result.current.closeAborted(21);
    });
    expect(view.result.current.pendingKind).toBeNull();
    expect(save.unfreeze).toHaveBeenCalled();
    expect(save.flushRemaining).toHaveBeenCalledOnce();
    expect(closePosts().filter((message) => message.result === 'ready')).toEqual([]);
  });

  it('does not reply ready after an aborted close flush settles', async () => {
    host();
    const hang = deferred<boolean>();
    const save = openSave({
      freeze: vi.fn(() => true),
      flushRemaining: vi.fn(() => hang.promise),
    });
    const view = renderHook(() => useNoteLeave(() => save));
    act(() => {
      view.result.current.prepareClose(12);
    });
    await act(async () => {
      await Promise.resolve();
    });
    act(() => {
      view.result.current.closeAborted(12);
    });
    expect(save.unfreeze).toHaveBeenCalled();
    expect(save.submitNow).not.toHaveBeenCalled();
    await act(async () => {
      hang.resolve(true);
      await hang.promise;
    });
    expect(closePosts().some((message) => message.result === 'ready')).toBe(false);
    expect(closePosts().some((message) => message.native_action === 'close')).toBe(false);
    expect(view.result.current.pendingKind).toBeNull();
  });

  it('submits an uncovered save when close aborts before the flush', async () => {
    host();
    const hang = deferred();
    const save = openSave({
      freeze: vi.fn(() => true),
      inFlightSerial: () => 2,
      waitInFlight: () => hang.promise,
      saveError: null,
      hasUnsaved: () => true,
    });
    const view = renderHook(() => useNoteLeave(() => save));
    act(() => {
      view.result.current.prepareClose(13);
    });
    act(() => {
      view.result.current.closeAborted(13);
    });
    expect(save.submitNow).toHaveBeenCalledOnce();
    expect(save.flushRemaining).not.toHaveBeenCalled();
    await act(async () => {
      hang.resolve();
      await hang.promise;
    });
    expect(closePosts()).toEqual([]);
  });

  it('clears only a matching deferred attempt and ignores an older abort', async () => {
    host();
    const hang = deferred();
    const save = openSave({
      waitInFlight: () => hang.promise,
      inFlightSerial: () => 1,
      saveError: null,
    });
    const view = renderHook(() => useNoteLeave(() => save));
    act(() => {
      void view.result.current.leaveNoteThen(vi.fn(() => true));
    });
    act(() => {
      view.result.current.prepareClose(14);
      view.result.current.prepareClose(15);
      view.result.current.closeAborted(14);
    });
    expect(view.result.current.holdsLeave).toBe(true);
    await act(async () => {
      hang.resolve();
      await hang.promise;
      await new Promise((resolve) => setTimeout(resolve, 0));
    });
    expect(closePosts()).toEqual([{
      native_action: 'close_reply',
      connection_id: 'view-4',
      attempt_id: 15,
      result: 'ready',
    }]);
  });
});

describe('deletion slot', () => {
  afterEach(() => {
    delete window.__CHA_NATIVE_POST__;
    delete window.__CHA_NATIVE_CONNECTION_ID__;
  });

  function host() {
    window.__CHA_NATIVE_CONNECTION_ID__ = 'view-4';
    window.__CHA_NATIVE_POST__ = vi.fn();
  }

  it('refuses navigation and a second deletion while delete holds the slot', async () => {
    const hang = deferred<boolean>();
    const save = openSave({ flushRemaining: () => hang.promise });
    const view = renderHook(() => useNoteLeave(() => save));
    let started: Promise<unknown> = Promise.resolve();
    act(() => {
      started = view.result.current.leaveNoteThen(async () => true);
    });
    await act(async () => { await Promise.resolve(); });
    expect(view.result.current.beginDelete()).toBe(false);
    const action = vi.fn(async () => true);
    await expect(view.result.current.leaveNoteThen(action)).resolves.toBe(false);
    expect(action).not.toHaveBeenCalled();
    await act(async () => {
      hang.resolve(true);
      await started;
    });
    expect(view.result.current.beginDelete()).toBe(true);
    expect(view.result.current.beginDelete()).toBe(false);
    const duringDelete = vi.fn(async () => true);
    await expect(view.result.current.leaveNoteThen(duringDelete)).resolves.toBe(false);
    expect(duringDelete).not.toHaveBeenCalled();
    act(() => view.result.current.endDelete());
    expect(view.result.current.pendingKind).toBeNull();
  });

  it('resumes the newest deferred close after deletion and ignores an aborted attempt', () => {
    host();
    const save = openSave({ hasUnsaved: () => false, inFlightSerial: () => null });
    const view = renderHook(() => useNoteLeave(() => save));
    act(() => {
      expect(view.result.current.beginDelete()).toBe(true);
      view.result.current.prepareClose(30);
      view.result.current.prepareClose(31);
      view.result.current.closeAborted(30);
    });
    expect(closePosts()).toEqual([]);
    expect(view.result.current.pendingKind).toBe('delete');
    act(() => view.result.current.endDelete());
    expect(closePosts()).toEqual([{
      native_action: 'close_reply',
      connection_id: 'view-4',
      attempt_id: 31,
      result: 'ready',
    }]);
  });

  it('does not resume a close aborted during deletion', () => {
    host();
    const save = openSave({ hasUnsaved: () => true });
    const view = renderHook(() => useNoteLeave(() => save));
    act(() => {
      view.result.current.beginDelete();
      view.result.current.prepareClose(32);
      view.result.current.closeAborted(32);
      view.result.current.endDelete();
    });
    expect(closePosts()).toEqual([]);
    expect(save.freeze).not.toHaveBeenCalled();
    expect(view.result.current.pendingKind).toBeNull();
  });
});

describe('captured leave URLs', () => {
  it('restores a captured address with replaceState', () => {
    window.history.replaceState(null, '', '/#/s/lobby/planning/');
    rememberAppUrl();
    const previous = rememberedAppUrl();
    window.history.replaceState(null, '', '/#/');
    rememberAppUrl();
    restoreAppUrl(previous);
    expect(window.location.hash).toBe('#/s/lobby/planning/');
    expect(rememberedAppUrl()).toBe(window.location.href);
  });
});
