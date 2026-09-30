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
  className = 'cha-dialog',
  initialFocus = 'confirm',
}: {
  confirmLabel: string;
  message: string;
  onCancel(): void;
  onConfirm(): void;
  title: string;
  className?: string;
  initialFocus?: 'cancel' | 'confirm';
}) {
  const dialog = useRef<HTMLDialogElement | null>(null);

  useEffect(() => {
    const element = dialog.current;
    if (!element) return;
    const previousFocus = document.activeElement;
    if (typeof element.showModal === 'function') element.showModal();
    else element.setAttribute('open', '');
    element.querySelector<HTMLButtonElement>(
      initialFocus === 'cancel' ? '.cha-button-ghost' : '.cha-button-danger',
    )?.focus();
    return () => {
      if (typeof element.close === 'function') element.close();
      if (previousFocus instanceof HTMLElement && previousFocus.isConnected) previousFocus.focus();
    };
  }, [initialFocus]);

  return createPortal(
    <dialog aria-label={title} className={className} onCancel={onCancel} ref={dialog}>
      <h2>{title}</h2>
      <p>{message}</p>
      <div className="cha-dialog-actions">
        <button className="cha-button cha-button-ghost" onClick={onCancel} type="button">
          Cancel
        </button>
        <button
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
