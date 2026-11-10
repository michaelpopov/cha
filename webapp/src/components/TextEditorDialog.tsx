import { useEffect, useRef, type FormEvent } from 'react';
import { createPortal } from 'react-dom';

import { TransliterationToggle, useTransliteration } from './TransliterationMode';

export function TextEditorDialog({
  error,
  loading,
  onCancel,
  onChange,
  onSave,
  placeholder = 'Enter or paste Markdown here',
  ready,
  saveDisabled = false,
  saving,
  textareaLabel,
  title,
  value,
}: {
  error: string | null;
  loading: boolean;
  onCancel(): void;
  onChange(value: string): void;
  onSave(): void;
  placeholder?: string | null;
  ready: boolean;
  saveDisabled?: boolean;
  saving: boolean;
  textareaLabel?: string;
  title: string;
  value: string;
}) {
  const dialog = useRef<HTMLDialogElement | null>(null);
  const transliteration = useTransliteration<HTMLTextAreaElement>(value);
  const editor = transliteration.field;

  useEffect(() => {
    const element = dialog.current;
    if (!element) return;
    if (typeof element.showModal === 'function') element.showModal();
    else element.setAttribute('open', '');
    return () => {
      if (typeof element.close === 'function') element.close();
    };
  }, []);

  useEffect(() => {
    if (ready) editor.current?.focus();
  }, [ready]);

  function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (ready && !loading && !saving && !saveDisabled) onSave();
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
          aria-label={textareaLabel ?? `${title} text`}
          autoFocus
          disabled={!ready || loading || saving}
          onChange={(event) => onChange(transliteration.convert(event, value))}
          placeholder={loading ? 'Loading…' : placeholder ?? undefined}
          ref={editor}
          spellCheck={false}
          value={value}
        />
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-dialog-actions">
          <TransliterationToggle
            disabled={!ready || loading || saving}
            transliteration={transliteration}
          />
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
            disabled={!ready || loading || saving || saveDisabled}
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
