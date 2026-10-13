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
