import { useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';

import { publicErrorMessage, type ChaClient } from '../api/client';

export function SystemPromptDialog({
  client, forumId, characterId, characterName, onClose,
}: {
  client: ChaClient;
  forumId: string;
  characterId: string;
  characterName: string;
  onClose(): void;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [prompt, setPrompt] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);

  useEffect(() => {
    const element = dialog.current;
    const previousFocus = document.activeElement;
    if (typeof element?.showModal === 'function') element.showModal();
    else element?.setAttribute('open', '');
    element?.querySelector<HTMLButtonElement>('[data-close]')?.focus();
    return () => {
      if (typeof element?.close === 'function') element.close();
      if (previousFocus instanceof HTMLElement && previousFocus.isConnected) previousFocus.focus();
    };
  }, []);

  useEffect(() => {
    let cancelled = false;
    setPrompt(null);
    setError(null);
    setCopied(false);
    void client.getForumSystemPrompt(forumId, characterId).then(
      ({ system_prompt }) => { if (!cancelled) setPrompt(system_prompt); },
      (failure: unknown) => {
        if (!cancelled) setError(publicErrorMessage(failure, 'The system prompt could not be loaded.'));
      },
    );
    return () => { cancelled = true; };
  }, [client, forumId, characterId]);

  async function copy() {
    if (prompt === null) return;
    try {
      await navigator.clipboard.writeText(prompt);
      setCopied(true);
      setError(null);
    } catch {
      setCopied(false);
      setError('The system prompt could not be copied. Select and copy the text manually.');
    }
  }

  return createPortal(
    <dialog
      aria-label={`System prompt for ${characterName}`}
      className="cha-dialog cha-system-prompt-dialog"
      onCancel={(event) => { event.preventDefault(); onClose(); }}
      ref={dialog}
    >
      {prompt === null && !error && <p role="status">Loading…</p>}
      {prompt !== null && <pre aria-label="System prompt" tabIndex={0}>{prompt}</pre>}
      {error && <p className="cha-error-message" role="alert">{error}</p>}
      <div className="cha-dialog-actions">
        <button aria-live="polite" className="cha-button cha-button-ghost" disabled={prompt === null} onClick={() => void copy()} type="button">{copied ? 'Copied' : 'Copy'}</button>
        <button className="cha-button cha-button-ghost" data-close onClick={onClose} type="button">Close</button>
      </div>
    </dialog>,
    document.body,
  );
}
