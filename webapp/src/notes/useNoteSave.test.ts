import type { Editor } from '@tiptap/core';
import { TextSelection } from '@tiptap/pm/state';
import { act, renderHook } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { ChaError } from '../api/client';
import { createNotesEditor } from './editor';
import { noteSaveDelayMs, useNoteSave } from './useNoteSave';

const open: Editor[] = [];

function deferred<T = void>() {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((ok, err) => {
    resolve = ok;
    reject = err;
  });
  return { promise, resolve, reject };
}

function insertText(created: Editor, text: string): void {
  created.view.dispatch(created.state.tr.insertText(text));
}

function setup(save: (document: Record<string, unknown>) => Promise<void>, contextKey: unknown = 'vault') {
  const created = createNotesEditor();
  const element = created.options.element;
  if (element instanceof HTMLElement && !element.isConnected) document.body.appendChild(element);
  open.push(created);
  const view = renderHook((props: { editor: Editor | null; noteId: number | null; contextKey: unknown }) => (
    useNoteSave({
      editor: props.editor,
      noteId: props.noteId,
      contextKey: props.contextKey,
      save,
    })
  ), { initialProps: { editor: created as Editor | null, noteId: 4 as number | null, contextKey } });
  return { editor: created, view };
}

beforeEach(() => {
  vi.useFakeTimers();
});

afterEach(() => {
  for (const item of open) item.destroy();
  open.length = 0;
  document.body.replaceChildren();
  vi.useRealTimers();
});

describe('note save scheduling', () => {
  it('sends the latest document five seconds after the first uncovered edit', async () => {
    const save = vi.fn<(document: Record<string, unknown>) => Promise<void>>(async () => undefined);
    const { editor, view } = setup(save);
    insertText(editor, 'a');
    insertText(editor, 'b');
    expect(save).not.toHaveBeenCalled();
    await act(async () => {
      await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
    });
    expect(save).toHaveBeenCalledOnce();
    expect(JSON.stringify(save.mock.calls[0][0])).toContain('ab');
    expect(view.result.current.editSerial()).toBe(2);
    expect(view.result.current.savedSerial()).toBe(2);
  });

  it('marks a save due when the deadline expires during a pending write', async () => {
    const first = deferred();
    const second = deferred();
    const save = vi.fn()
      .mockImplementationOnce(() => first.promise)
      .mockImplementationOnce(() => second.promise);
    const { editor, view } = setup(save);
    insertText(editor, 'a');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
    });
    expect(save).toHaveBeenCalledOnce();
    insertText(editor, 'b');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
    });
    expect(save).toHaveBeenCalledOnce();
    expect(view.result.current.isSaveDue()).toBe(true);
    await act(async () => {
      first.resolve();
      await first.promise;
    });
    expect(save).toHaveBeenCalledTimes(2);
    expect(view.result.current.savedSerial()).toBe(1);
    await act(async () => {
      second.resolve();
      await second.promise;
    });
    expect(view.result.current.savedSerial()).toBe(2);
  });

  it('covers only the committed serial and keeps later edits unsaved', async () => {
    const first = deferred();
    const save = vi.fn(() => first.promise);
    const { editor, view } = setup(save);
    insertText(editor, 'a');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
    });
    insertText(editor, 'b');
    await act(async () => {
      first.resolve();
      await first.promise;
    });
    expect(view.result.current.savedSerial()).toBe(1);
    expect(view.result.current.editSerial()).toBe(2);
    expect(view.result.current.hasUnsaved()).toBe(true);
  });

  it('sends after a Retry requested during a failing pending write', async () => {
    const first = deferred();
    const second = deferred();
    const save = vi.fn()
      .mockImplementationOnce(() => first.promise)
      .mockImplementationOnce(() => second.promise);
    const { editor, view } = setup(save);
    insertText(editor, 'a');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
    });
    act(() => {
      view.result.current.retry();
    });
    expect(view.result.current.saveError).toBeNull();
    await act(async () => {
      first.reject(new ChaError('internal_error', 'The note could not be saved.'));
      await first.promise.catch(() => undefined);
    });
    expect(view.result.current.saveError?.serial).toBe(1);
    expect(save).toHaveBeenCalledTimes(2);
    await act(async () => {
      second.resolve();
      await second.promise;
    });
    expect(view.result.current.saveError).toBeNull();
    expect(view.result.current.savedSerial()).toBe(1);
  });

  it('reports the failed serial on the handle captured before the reply', async () => {
    const first = deferred();
    const save = vi.fn(() => first.promise);
    const { editor, view } = setup(save);
    insertText(editor, 'a');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
    });
    const captured = view.result.current;
    expect(captured.saveError).toBeNull();
    const seen = captured.waitInFlight().then(() => captured.saveError?.serial);
    await act(async () => {
      first.reject(new ChaError('internal_error', 'The note could not be saved.'));
      await expect(seen).resolves.toBe(1);
    });
    expect(save).toHaveBeenCalledOnce();
  });

  it('keeps the error visible until a later success covers it', async () => {
    const first = deferred();
    const second = deferred();
    const save = vi.fn()
      .mockImplementationOnce(() => first.promise)
      .mockImplementationOnce(() => second.promise);
    const { editor, view } = setup(save);
    insertText(editor, 'a');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
    });
    await act(async () => {
      first.reject(new ChaError('internal_error', 'The note could not be saved.'));
      await first.promise.catch(() => undefined);
    });
    expect(view.result.current.saveError?.message).toBe('The note could not be saved.');
    expect(save).toHaveBeenCalledOnce();
    insertText(editor, 'b');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
    });
    expect(save).toHaveBeenCalledTimes(2);
    expect(view.result.current.saveError?.serial).toBe(1);
    await act(async () => {
      second.resolve();
      await second.promise;
    });
    expect(view.result.current.saveError).toBeNull();
    expect(view.result.current.savedSerial()).toBe(2);
  });

  it('does not start a retry loop from failure alone', async () => {
    const save = vi.fn(async () => {
      throw new ChaError('internal_error', 'The note could not be saved.');
    });
    const { editor, view } = setup(save);
    insertText(editor, 'a');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
    });
    expect(save).toHaveBeenCalledOnce();
    await act(async () => {
      await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
    });
    expect(save).toHaveBeenCalledOnce();
    expect(view.result.current.saveError?.serial).toBe(1);
  });

  it('counts Undo during a write as a later unsaved serial', async () => {
    const first = deferred();
    const save = vi.fn(() => first.promise);
    const { editor, view } = setup(save);
    insertText(editor, 'a');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
    });
    editor.commands.undo();
    expect(view.result.current.editSerial()).toBe(2);
    await act(async () => {
      first.resolve();
      await first.promise;
    });
    expect(view.result.current.savedSerial()).toBe(1);
    expect(view.result.current.hasUnsaved()).toBe(true);
  });

  it('ignores a stale reply after the editor context changes', async () => {
    const first = deferred();
    const save = vi.fn(() => first.promise);
    const { editor, view } = setup(save, 'vault-a');
    insertText(editor, 'a');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
    });
    act(() => {
      view.rerender({ editor, noteId: 4, contextKey: 'vault-b' });
    });
    await act(async () => {
      first.resolve();
      await first.promise;
    });
    expect(view.result.current.savedSerial()).toBe(0);
    expect(view.result.current.saveError).toBeNull();
  });

  it('does not save a selection-only change', async () => {
    const save = vi.fn(async () => undefined);
    const { editor, view } = setup(save);
    insertText(editor, 'ab');
    await act(async () => {
      await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
    });
    save.mockClear();
    editor.view.dispatch(editor.state.tr.setSelection(
      TextSelection.create(editor.state.doc, 1),
    ));
    await act(async () => {
      await vi.advanceTimersByTimeAsync(noteSaveDelayMs);
    });
    expect(save).not.toHaveBeenCalled();
    expect(view.result.current.editSerial()).toBe(1);
  });
});
