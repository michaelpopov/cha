import { useEffect, useRef } from 'react';
import { createPortal } from 'react-dom';

// CHA runs inside a WKWebView, whose UI delegate implements no JavaScript panel
// methods. window.confirm there is not merely unstyled: no panel appears and the
// call returns false, so a button guarded by it does nothing at all. Asking in
// the page is the only confirmation the reader ever sees.
export function ConfirmDialog({
  confirmLabel,
  message,
  onCancel,
  onConfirm,
  title,
}: {
  confirmLabel: string;
  message: string;
  onCancel(): void;
  onConfirm(): void;
  title: string;
}) {
  const dialog = useRef<HTMLDialogElement | null>(null);

  useEffect(() => {
    if (typeof dialog.current?.showModal === 'function') dialog.current.showModal();
    else dialog.current?.setAttribute('open', '');
    return () => {
      if (typeof dialog.current?.close === 'function') dialog.current.close();
    };
  }, []);

  return createPortal(
    <dialog className="cha-dialog" onCancel={onCancel} ref={dialog}>
      <h2>{title}</h2>
      <p>{message}</p>
      <div className="cha-dialog-actions">
        <button className="cha-button cha-button-ghost" onClick={onCancel} type="button">
          Cancel
        </button>
        <button
          autoFocus
          className="cha-button cha-button-danger"
          onClick={onConfirm}
          type="button"
        >
          {confirmLabel}
        </button>
      </div>
    </dialog>,
    document.body,
  );
}
