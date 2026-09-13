import { useEffect, useRef, useState, type FormEvent } from 'react';
import { createPortal } from 'react-dom';

export function PasswordDialog({
  error,
  name,
  onCancel,
  onSubmit,
}: {
  error: string | null;
  name: string;
  onCancel(): void;
  onSubmit(password: string): void;
}) {
  const dialog = useRef<HTMLDialogElement | null>(null);
  const [password, setPassword] = useState('');

  useEffect(() => {
    if (typeof dialog.current?.showModal === 'function') dialog.current.showModal();
    else dialog.current?.setAttribute('open', '');
    return () => {
      if (typeof dialog.current?.close === 'function') dialog.current.close();
    };
  }, []);

  function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (password) onSubmit(password);
  }

  return createPortal(
    <dialog className="cha-dialog" onCancel={onCancel} ref={dialog}>
      <form onSubmit={submit}>
        <h2>Open {name}</h2>
        <label>
          Password
          <input
            autoFocus
            autoComplete="current-password"
            className="cha-form-control"
            onChange={(event) => setPassword(event.target.value)}
            type="password"
            value={password}
          />
        </label>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-dialog-actions">
          <button className="cha-button cha-button-ghost" onClick={onCancel} type="button">Cancel</button>
          <button className="cha-button cha-button-primary" disabled={!password} type="submit">Open vault</button>
        </div>
      </form>
    </dialog>,
    document.body,
  );
}
