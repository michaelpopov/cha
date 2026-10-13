import { useCallback, useEffect, useRef, useState } from 'react';
import type { Editor } from '@tiptap/core';

import { publicErrorMessage } from '../api/client';

export const noteSaveDelayMs = 5_000;

export type NoteSaveError = {
  serial: number;
  message: string;
};

export type NoteSaveHandle = {
  isOpen: boolean;
  frozen: boolean;
  saveError: NoteSaveError | null;
  hasUnsaved(): boolean;
  inFlightSerial(): number | null;
  waitInFlight(): Promise<void>;
  freeze(): boolean;
  unfreeze(): void;
  submitNow(): void;
  flushRemaining(): Promise<boolean>;
  dispose(): void;
};

type Origin = {
  editor: Editor;
  noteId: number;
  contextKey: unknown;
};

type InFlight = {
  serial: number;
  done: Promise<void>;
};

const saveFailedMessage = 'The note could not be saved.';

export function useNoteSave({
  editor,
  noteId,
  contextKey,
  save,
}: {
  editor: Editor | null;
  noteId: number | null;
  contextKey: unknown;
  save: (document: Record<string, unknown>) => Promise<void>;
}): NoteSaveHandle {
  const editorRef = useRef(editor);
  const noteIdRef = useRef(noteId);
  const contextRef = useRef(contextKey);
  const saveRef = useRef(save);
  editorRef.current = editor;
  noteIdRef.current = noteId;
  contextRef.current = contextKey;
  saveRef.current = save;

  const editSerial = useRef(0);
  const savedSerial = useRef(0);
  const inFlight = useRef<InFlight | null>(null);
  const deadline = useRef<ReturnType<typeof setTimeout> | null>(null);
  const deadlineGen = useRef(0);
  const saveDue = useRef(false);
  const frozenRef = useRef(false);
  const [frozen, setFrozen] = useState(false);
  const [saveError, setSaveError] = useState<NoteSaveError | null>(null);
  const saveErrorRef = useRef<NoteSaveError | null>(null);
  saveErrorRef.current = saveError;

  const sendRef = useRef<(allowFrozen: boolean) => void>(() => undefined);

  const clearDeadline = useCallback(() => {
    deadlineGen.current += 1;
    if (deadline.current !== null) {
      clearTimeout(deadline.current);
      deadline.current = null;
    }
  }, []);

  sendRef.current = (allowFrozen) => {
    if (inFlight.current) return;
    if (frozenRef.current && !allowFrozen) return;
    if (editSerial.current <= savedSerial.current) return;
    const currentEditor = editorRef.current;
    const currentNoteId = noteIdRef.current;
    if (!currentEditor || currentEditor.isDestroyed || currentNoteId === null) return;
    const origin: Origin = {
      editor: currentEditor,
      noteId: currentNoteId,
      contextKey: contextRef.current,
    };
    const serial = editSerial.current;
    const document = currentEditor.getJSON() as Record<string, unknown>;
    const done = saveRef.current(document).then(
      () => settle(serial, origin, null),
      (failure: unknown) => settle(serial, origin, failure),
    );
    inFlight.current = { serial, done };
  };

  function settle(serial: number, origin: Origin, failure: unknown) {
    if (inFlight.current?.serial === serial) inFlight.current = null;
    if (
      editorRef.current !== origin.editor
      || noteIdRef.current !== origin.noteId
      || contextRef.current !== origin.contextKey
    ) return;
    if (failure == null) {
      savedSerial.current = serial;
      if (saveErrorRef.current && saveErrorRef.current.serial <= serial) {
        saveErrorRef.current = null;
        setSaveError(null);
      }
    } else {
      const next = {
        serial,
        message: publicErrorMessage(failure, saveFailedMessage),
      };
      saveErrorRef.current = next;
      setSaveError(next);
    }
    if (!saveDue.current) return;
    saveDue.current = false;
    if (editSerial.current > savedSerial.current && !frozenRef.current) sendRef.current(false);
  }

  const startDeadline = useCallback(() => {
    if (deadline.current !== null || saveDue.current || frozenRef.current) return;
    const gen = deadlineGen.current + 1;
    deadlineGen.current = gen;
    deadline.current = setTimeout(() => {
      if (deadlineGen.current !== gen) return;
      deadline.current = null;
      if (inFlight.current) {
        saveDue.current = true;
        return;
      }
      sendRef.current(false);
    }, noteSaveDelayMs);
  }, []);

  const noteAccepted = useCallback(() => {
    if (frozenRef.current) return;
    const current = editorRef.current;
    if (!current || current.isDestroyed || !current.isEditable) return;
    editSerial.current += 1;
    startDeadline();
  }, [startDeadline]);

  useEffect(() => {
    editSerial.current = 0;
    savedSerial.current = 0;
    saveDue.current = false;
    clearDeadline();
    saveErrorRef.current = null;
    setSaveError(null);
    frozenRef.current = false;
    setFrozen(false);
  }, [clearDeadline, editor, noteId, contextKey]);

  useEffect(() => {
    if (!editor) return;
    const handler = () => noteAccepted();
    editor.on('update', handler);
    return () => {
      editor.off('update', handler);
    };
  }, [editor, noteAccepted]);

  const waitInFlight = useCallback(async () => {
    const pending = inFlight.current;
    if (pending) await pending.done;
  }, []);

  const freeze = useCallback(() => {
    frozenRef.current = true;
    setFrozen(true);
    const current = editorRef.current;
    if (current && !current.isDestroyed) current.setEditable(false, false);
    let canceled = false;
    if (deadline.current !== null) {
      clearDeadline();
      canceled = true;
    }
    if (saveDue.current) {
      saveDue.current = false;
      canceled = true;
    }
    return canceled && editSerial.current > savedSerial.current;
  }, [clearDeadline]);

  const unfreeze = useCallback(() => {
    frozenRef.current = false;
    setFrozen(false);
    const current = editorRef.current;
    if (current && !current.isDestroyed) current.setEditable(true, false);
  }, []);

  const submitNow = useCallback(() => {
    clearDeadline();
    if (inFlight.current) {
      saveDue.current = true;
      return;
    }
    sendRef.current(false);
  }, [clearDeadline]);

  const flushRemaining = useCallback(async () => {
    if (editSerial.current <= savedSerial.current) return true;
    const serial = editSerial.current;
    sendRef.current(true);
    await waitInFlight();
    return savedSerial.current >= serial;
  }, [waitInFlight]);

  const dispose = useCallback(() => {
    clearDeadline();
    saveDue.current = false;
    frozenRef.current = false;
    setFrozen(false);
    saveErrorRef.current = null;
    setSaveError(null);
    editSerial.current = 0;
    savedSerial.current = 0;
  }, [clearDeadline]);

  return {
    isOpen: editor !== null && !editor.isDestroyed && noteId !== null,
    frozen,
    get saveError() {
      return saveErrorRef.current;
    },
    hasUnsaved: () => editSerial.current > savedSerial.current,
    inFlightSerial: () => inFlight.current?.serial ?? null,
    waitInFlight,
    freeze,
    unfreeze,
    submitNow,
    flushRemaining,
    dispose,
  };
}
