import { useRef } from 'react';
import { createPortal } from 'react-dom';

import { useDialogElement } from './useDialogElement';

export type NoteCompressionProgress = {
  message: string;
  count?: string;
  cancelEnabled: boolean;
};

export function NoteCompressionDialog({ progress, onCancel }: {
  progress: NoteCompressionProgress;
  onCancel(): void;
}) {
  const cancel = useRef<HTMLButtonElement>(null);
  const dialog = useDialogElement(() => cancel.current?.focus());
  return createPortal(
    <dialog
      aria-label="Note compression"
      className="cha-dialog"
      onCancel={event => {
        event.preventDefault();
        if (progress.cancelEnabled) onCancel();
      }}
      onKeyDown={event => event.stopPropagation()}
      ref={dialog}
    >
      <div className="cha-note-compression-status" role="status">
        <span aria-hidden="true" className="cha-notes-import-spinner" />
        <div>{progress.message}{progress.count && <div>{progress.count}</div>}</div>
      </div>
      <div className="cha-dialog-actions">
        <button className="cha-button cha-button-ghost" disabled={!progress.cancelEnabled}
          onClick={onCancel} ref={cancel} type="button">Cancel</button>
      </div>
    </dialog>, document.body,
  );
}
