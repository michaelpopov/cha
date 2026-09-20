import { useEffect, useRef, useState, type ChangeEvent, type FormEvent } from 'react';

import { publicErrorMessage } from '../api/client';
import { ConfirmDialog } from './ConfirmDialog';
import { CheckIcon, CloseIcon, EditIcon, FileUpIcon, SkullBonesIcon, TextLinesIcon } from './Icons';
import { TextEditorDialog } from './TextEditorDialog';
import { TransliterationToggle, useTransliteration } from './TransliterationMode';

interface EditableTitleProps {
  available: boolean;
  disabled?: boolean;
  id: string | null;
  name: string | undefined;
  onSave(displayName: string): Promise<void>;
  subject: string;
}

export function EditableTitle({ available, disabled = false, id, name, onSave, subject }: EditableTitleProps) {
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState(name ?? '');
  const transliteration = useTransliteration<HTMLInputElement>(draft);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setEditing(false);
    setDraft(name ?? '');
    setError(null);
  }, [id, name]);

  if (!id || !name) return null;
  if (!available) return <h1>{name}</h1>;

  function cancel() {
    setDraft(name ?? '');
    setEditing(false);
    setError(null);
  }

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    const displayName = draft.trim();
    if (!displayName || saving || disabled) return;
    if (displayName === name) {
      setEditing(false);
      return;
    }
    setSaving(true);
    setError(null);
    try {
      await onSave(displayName);
      setEditing(false);
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, `${subject} name could not be saved.`));
    } finally {
      setSaving(false);
    }
  }

  if (!editing) {
    return (
      <button
        aria-label={`Rename ${name}`}
        className="cha-persona-title-trigger"
        disabled={disabled}
        onClick={() => setEditing(true)}
        type="button"
      >
        <span>{name}</span>
        <EditIcon />
      </button>
    );
  }

  const lowerSubject = subject.toLowerCase();
  return (
    <form className="cha-persona-title-form" onSubmit={(event) => void save(event)}>
      <input
        aria-label={`${subject} name`}
        autoFocus
        disabled={saving || disabled}
        onChange={(event) => setDraft(transliteration.convert(event, draft))}
        onFocus={(event) => event.currentTarget.select()}
        onKeyDown={(event) => {
          if (event.key === 'Escape') cancel();
        }}
        ref={transliteration.field}
        value={draft}
      />
      <TransliterationToggle disabled={saving || disabled} transliteration={transliteration} />
      <button
        aria-label={`Save ${lowerSubject} name`}
        className="cha-title-icon-action"
        disabled={saving || disabled || draft.trim() === ''}
        type="submit"
      >
        <CheckIcon />
      </button>
      <button
        aria-label="Cancel renaming"
        className="cha-title-icon-action"
        disabled={saving || disabled}
        onClick={cancel}
        type="button"
      >
        <CloseIcon />
      </button>
      {error && <span className="cha-persona-title-error" role="alert">{error}</span>}
    </form>
  );
}

interface DefinitionUploadProps {
  ariaLabel: string;
  failureMessage: string;
  id: string;
  onUpload(markdown: string): Promise<void>;
}

function DefinitionUpload({ ariaLabel, failureMessage, id, onUpload }: DefinitionUploadProps) {
  const input = useRef<HTMLInputElement>(null);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => setError(null), [id]);

  async function replaceFromFile(event: ChangeEvent<HTMLInputElement>) {
    const file = event.target.files?.[0];
    if (!file || saving) return;
    setSaving(true);
    setError(null);
    try {
      await onUpload(await file.text());
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, failureMessage));
    } finally {
      setSaving(false);
      if (input.current) input.current.value = '';
    }
  }

  return (
    <div className="cha-definition-upload">
      <input
        accept=".md,.txt,text/markdown,text/plain"
        className="cha-file-input"
        onChange={(event) => void replaceFromFile(event)}
        ref={input}
        type="file"
      />
      <button
        aria-label={ariaLabel}
        className="cha-compact-icon-action"
        disabled={saving}
        onClick={() => input.current?.click()}
        type="button"
      >
        <FileUpIcon />
      </button>
      {error && <span className="cha-definition-upload-error" role="alert">{error}</span>}
    </div>
  );
}

interface DetailActionsProps {
  name: string;
  subject: string;
  deleteMessage: string;
  onDelete(): Promise<void>;
  editor?: {
    title: string;
    value: string;
    uploadLabel: string;
    onSave(markdown: string): Promise<void>;
  };
}

// The screen owns the data and mutations; these controls only own their dialogs.
export function DetailActions({ name, subject, deleteMessage, onDelete, editor }: DetailActionsProps) {
  const [confirming, setConfirming] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [deleteError, setDeleteError] = useState<string | null>(null);
  const [text, setText] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [editorError, setEditorError] = useState<string | null>(null);

  async function remove() {
    if (deleting) return;
    setConfirming(false);
    setDeleting(true);
    setDeleteError(null);
    try {
      await onDelete();
    } catch (failure: unknown) {
      setDeleteError(publicErrorMessage(failure, `${subject} could not be deleted.`));
    } finally {
      setDeleting(false);
    }
  }

  async function save() {
    if (!editor || text === null || saving) return;
    setSaving(true);
    setEditorError(null);
    try {
      await editor.onSave(text);
      setText(null);
    } catch (failure: unknown) {
      setEditorError(publicErrorMessage(failure, `${editor.title} could not be saved.`));
    } finally {
      setSaving(false);
    }
  }

  return (
    <div className="cha-detail-action-buttons">
      {editor && <>
        <button
          aria-label={editor.title}
          className="cha-compact-icon-action"
          onClick={() => { setText(editor.value); setEditorError(null); }}
          title={editor.title}
          type="button"
        ><TextLinesIcon /></button>
        <DefinitionUpload
          ariaLabel={editor.uploadLabel}
          failureMessage={`${subject} could not be replaced.`}
          id={name}
          onUpload={editor.onSave}
        />
      </>}
      <button
        aria-label={`Delete ${name}`}
        className="cha-compact-icon-action cha-danger-icon-action"
        disabled={deleting}
        onClick={() => setConfirming(true)}
        title={`Delete ${name}`}
        type="button"
      ><SkullBonesIcon /></button>
      {deleteError && <span className="cha-definition-upload-error" role="alert">{deleteError}</span>}
      {confirming && <ConfirmDialog
        confirmLabel={`Delete ${subject.toLowerCase()}`}
        message={deleteMessage}
        onCancel={() => setConfirming(false)}
        onConfirm={() => void remove()}
        title={`Delete ${subject.toLowerCase()}?`}
      />}
      {text !== null && editor && <TextEditorDialog
        error={editorError}
        loading={false}
        onCancel={() => setText(null)}
        onChange={setText}
        onSave={() => void save()}
        ready
        saving={saving}
        title={editor.title}
        value={text}
      />}
    </div>
  );
}
