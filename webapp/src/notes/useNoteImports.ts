import type { Editor } from '@tiptap/core';
import { useReducer, useRef } from 'react';

import { ChaError, publicErrorMessage, type NoteItem, type NotesClient } from '../api/client';
import { readTextFile } from '../textFile';
import { appendNoteHtml, createNotesEditor, insertImportedDocument, replaceNoteHtml } from './editor';
import { noteFileHtml, noteFileTooLarge } from './fileImport';
import { notePageHtml, notePageName } from './pageImport';
import { availableNoteName } from './tree';
import type { useNoteLeave } from './useNoteLeave';
import type { NoteSaveHandle } from './useNoteSave';

type ImportState = { stage: string | null; error: string | null; replacing: boolean };
type ImportTarget = { noteId: number; documentId: number };
type Source = { kind: 'page'; url: string } | { kind: 'file'; file: File | string; explicit: boolean };

type Options = {
  client: NotesClient;
  epoch: number;
  leave: Pick<ReturnType<typeof useNoteLeave>, 'beginNoteAction' | 'endNoteAction' | 'whenIdle'>;
  openNote(): { noteId: number | null; documentId: number | null; editor: Editor | null; save: NoteSaveHandle | null };
  waitForOpen(): Promise<void> | null;
  updated(item: NoteItem): void;
  refresh(): void;
};

// The app owns these runs; opening or hiding an editor does not retire them.
// A new notes epoch retires them.
export function useNoteImports(options: Options) {
  const latest = useRef(options);
  latest.current = options;
  const [, render] = useReducer((revision: number) => revision + 1, 0);
  const scope = useRef({ epoch: options.epoch, cancel: new AbortController(), runs: new Map<number, ImportState>() });
  if (scope.current.epoch !== options.epoch) {
    scope.current.cancel.abort();
    scope.current = { epoch: options.epoch, cancel: new AbortController(), runs: new Map() };
  }

  function start({ noteId, documentId }: ImportTarget, source: Source): Promise<boolean | null> {
    const owner = scope.current;
    if (owner.runs.get(documentId)?.stage != null) return Promise.resolve(false);
    const { client } = latest.current;
    const wireEpoch = client.contextEpoch();
    const current = () => {
      try { return !owner.cancel.signal.aborted && client.contextEpoch() === wireEpoch; }
      catch { return false; }
    };
    const run: ImportState = { stage: 'Retrieving content…', error: null, replacing: false };
    owner.runs.set(documentId, run);
    const stage = (text: string) => { if (current()) { run.stage = text; render(); } };
    const warning = (text: string) => { if (current()) { run.error = text; render(); } };
    render();
    return (async () => {
      let prepared: Editor | undefined;
      let target: Editor | undefined;
      let save: NoteSaveHandle | null = null;
      let canceledSave = false;
      let guarded = false;
      let locked = false;
      const liveTarget = () => target && !target.isDestroyed && latest.current.openNote().editor === target;
      try {
        let html: string;
        let title: string;
        let preserveWhitespace = false;
        if (source.kind === 'page') {
          const page = await client.readNotePage(source.url, value => stage(
            value === 'formatting_transcript' ? 'Formatting transcript…' : 'Formatting article…'));
          if (!current()) return false;
          html = notePageHtml(page);
          title = notePageName(page.title);
        } else {
          let file;
          if (typeof source.file === 'string') file = await client.readNoteTextFile(source.file);
          else {
            try { file = { name: source.file.name, text: await readTextFile(source.file, noteFileTooLarge) }; }
            catch (failure) { throw new ChaError('invalid_argument', (failure as Error).message); }
          }
          if (!current()) return false;
          if (!file) {
            if (source.explicit) warning('The file could not be loaded. Check the path and permissions.');
            return null;
          }
          ({ html, title, preserveWhitespace } = noteFileHtml(file));
          // The file replaces the note, so its editor must not accept edits now.
          run.replacing = true;
          render();
        }
        const rejected = (message: string) => { throw new ChaError('invalid_argument', message); };
        prepared = createNotesEditor({ readPicture: client.readNotePicture, onCommandError: rejected });
        const readPicture = (url: string) => { stage('Loading pictures…'); return client.readNotePicture(url); };
        const accepted = source.kind === 'page'
          ? await appendNoteHtml(prepared, html, readPicture, current)
          : await replaceNoteHtml(prepared, html, readPicture, current, preserveWhitespace);
        if (!accepted || !current()) return false;

        // Only the short merge/save phase shares the existing note-action lock.
        // Retrieval and formatting can run concurrently with navigation and other imports.
        stage('Saving note…');
        while (!latest.current.leave.beginNoteAction()) {
          await latest.current.leave.whenIdle(owner.cancel.signal);
          if (!current()) return false;
        }
        guarded = true;
        if (!current()) return false;
        const items = await client.listNoteItems();
        if (!current()) return false;
        const note = items.find(item => item.id === noteId);
        if (!note) throw new ChaError('not_found', 'This note is no longer available.');
        if (note.read_only) throw new ChaError('invalid_argument', 'The note is read-only.');
        // An opening may resolve to either row. Let it settle before deciding
        // whether to merge through the live editor.
        if (latest.current.openNote().noteId === noteId) await latest.current.waitForOpen();
        if (!current()) return false;
        const open = latest.current.openNote();
        if (open.documentId === documentId && open.editor && !open.editor.isDestroyed && open.save?.isOpen) {
          target = open.editor;
          save = open.save;
          canceledSave = save.freeze();
          await save.waitInFlight();
          if (!current()) return false;
          // The insertion must reach the normal save tracker before it is frozen again.
          if (liveTarget()) save.unfreeze(true);
          else {
            target = undefined;
            save = null;
          }
        }
        if (!target) {
          const content = await client.getNote(noteId, documentId === noteId ? 'full_text' : 'compressed');
          if (!current()) return false;
          if (content.document_id !== documentId) throw new ChaError('not_found', 'This note document is no longer available.');
          target = createNotesEditor({ content: content.document, readPicture: client.readNotePicture, onCommandError: rejected });
        }
        const wasEmpty = target.isEmpty;
        const inserted = insertImportedDocument(target, prepared.getJSON(), source.kind === 'file');
        if (save) save.freeze();
        if (!inserted) {
          // The open editor already reported why it rejected the content.
          if (save) return false;
          throw new ChaError('invalid_argument', 'The content could not be imported into the note.');
        }
        if (save) {
          if (!await save.flushRemaining()) throw new ChaError('internal_error', 'The note could not be saved.');
        } else {
          const result = await client.saveNote(documentId, target.getJSON());
          if (current() && result?.annotation_created) latest.current.refresh();
        }
        if (!current()) return false;
        if (title && /^New note(?: [1-9]\d*)?$/u.test(note.name) && (source.kind === 'file' || wasEmpty)) {
          try {
            const renamed = await client.renameNoteItem(noteId, availableNoteName(items, note.parent_id, title, noteId));
            if (!current()) return false;
            latest.current.updated(renamed);
          } catch (failure) {
            warning(`The ${source.kind} was imported, but the note could not be renamed. `
              + publicErrorMessage(failure, 'Try renaming the note again.'));
          }
        }
        if (!current()) return false;
        if (source.kind === 'file' || wasEmpty) {
          try {
            const item = await client.setNoteReadOnly(noteId, true);
            if (!current()) return false;
            locked = true;
            latest.current.updated(item);
          } catch (failure) {
            warning('The content was imported, but the note could not be made read-only. '
              + publicErrorMessage(failure, 'Try Make read-only from the note menu.'));
          }
        }
        return true;
      } catch (failure) {
        warning(publicErrorMessage(failure, source.kind === 'page'
          ? 'Could not retrieve the web page. Try again.' : 'The file could not be loaded. Try again.'));
        return false;
      } finally {
        prepared?.destroy();
        if (save && current() && liveTarget()) {
          save.unfreeze(!locked);
          // A failed merge must not leave earlier edits without their canceled save.
          if (canceledSave && save.hasUnsaved() && !save.saveError) save.submitNow();
        }
        if (target && !save) target.destroy();
        if (guarded) latest.current.leave.endNoteAction();
        if (!owner.cancel.signal.aborted) {
          run.stage = null;
          if (!run.error) owner.runs.delete(documentId);
          render();
        }
      }
    })();
  }

  return {
    state: (id: number | null): ImportState | undefined => id === null ? undefined : scope.current.runs.get(id),
    dismiss(id: number) {
      const run = scope.current.runs.get(id);
      if (run) {
        run.error = null;
        if (run.stage === null) scope.current.runs.delete(id);
        render();
      }
    },
    page: (target: ImportTarget, url: string) => start(target, { kind: 'page', url }),
    file: (target: ImportTarget, file: File | string, explicit = false) => start(target, { kind: 'file', file, explicit }),
  };
}

export type NoteImports = ReturnType<typeof useNoteImports>;
