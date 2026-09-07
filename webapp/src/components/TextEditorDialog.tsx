import { useEffect, useRef, type FormEvent } from 'react';
import { createPortal } from 'react-dom';

export function TextEditorDialog({
  error,
  loading,
  onCancel,
  onChange,
  onSave,
  ready,
  saving,
  title,
  value,
}: {
  error: string | null;
  loading: boolean;
  onCancel(): void;
  onChange(value: string): void;
  onSave(): void;
  ready: boolean;
  saving: boolean;
  title: string;
  value: string;
}) {
  const dialog = useRef<HTMLDialogElement | null>(null);
  const editor = useRef<HTMLTextAreaElement | null>(null);

  useEffect(() => {
    if (typeof dialog.current?.showModal === 'function') dialog.current.showModal();
    else dialog.current?.setAttribute('open', '');
    return () => {
      if (typeof dialog.current?.close === 'function') dialog.current.close();
    };
  }, []);

  useEffect(() => {
    if (ready) editor.current?.focus();
  }, [ready]);

  function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (ready && !loading && !saving) onSave();
  }

  return createPortal(
    <dialog
      className="cha-dialog cha-text-editor-dialog"
      onCancel={(event) => {
        event.preventDefault();
        if (!saving) onCancel();
      }}
      ref={dialog}
    >
      <form onSubmit={submit}>
        <h2>{title}</h2>
        <textarea
          aria-label={`${title} text`}
          autoFocus
          disabled={!ready || loading || saving}
          onChange={(event) => onChange(event.target.value)}
          placeholder={loading ? 'Loading…' : 'Enter or paste Markdown here'}
          ref={editor}
          spellCheck={false}
          value={value}
        />
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-dialog-actions">
          <button
            className="cha-button cha-button-ghost cha-text-editor-clear"
            disabled={!ready || loading || saving || value === ''}
            onClick={() => onChange('')}
            type="button"
          >
            Clear
          </button>
          <button
            className="cha-button cha-button-ghost"
            disabled={saving}
            onClick={onCancel}
            type="button"
          >
            Cancel
          </button>
          <button
            className="cha-button cha-button-primary"
            disabled={!ready || loading || saving}
            type="submit"
          >
            {saving ? 'Saving…' : 'Save'}
          </button>
        </div>
      </form>
    </dialog>,
    document.body,
  );
}
