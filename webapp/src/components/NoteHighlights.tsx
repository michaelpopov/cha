import type { Editor } from '@tiptap/core';
import { useEffect, useRef, useState } from 'react';

import { noteHighlights, type NoteHighlight } from '../notes/highlights';
import { BookmarkIcon } from './Icons';
import { menuKeys } from './NotesTree';

export function NoteHighlights({ editor, frozen, onChoose, onClose }: {
  editor: Editor;
  frozen: boolean;
  onChoose(highlight: NoteHighlight): void;
  onClose(): void;
}) {
  const [open, setOpen] = useState(false);
  const root = useRef<HTMLDivElement | null>(null);
  const menu = useRef<HTMLDivElement | null>(null);
  const visible = open && !frozen && !editor.isDestroyed;

  useEffect(() => {
    if (!visible) {
      setOpen(false);
      return;
    }
    const first = menu.current?.querySelector<HTMLButtonElement>('button');
    (first || menu.current)?.focus({ preventScroll: true });
    const dismiss = (event: PointerEvent) => {
      if (event.target instanceof Node && root.current?.contains(event.target)) return;
      setOpen(false);
    };
    window.addEventListener('pointerdown', dismiss);
    return () => {
      window.removeEventListener('pointerdown', dismiss);
    };
  }, [visible]);

  const entries = visible ? noteHighlights(editor.state.doc) : [];

  return (
    <div className="cha-notes-highlights" ref={root}>
      <button
        aria-label="Highlights"
        aria-expanded={visible}
        aria-haspopup="menu"
        className="cha-button"
        title="Highlights"
        disabled={frozen || editor.isDestroyed}
        onClick={() => setOpen((value) => !value)}
        type="button"
      >
        <BookmarkIcon />
      </button>
      {visible && (
        <div
          aria-label="Highlights"
          className="cha-notes-list-menu cha-notes-heading-menu cha-notes-highlights-menu"
          onKeyDown={(event) => menuKeys(event, () => {
            setOpen(false);
            onClose();
          })}
          ref={menu}
          role="menu"
          tabIndex={-1}
        >
          {entries.length === 0 && <p role="status">No highlights</p>}
          {entries.map((entry) => (
            <button
              key={entry.from}
              onClick={() => {
                setOpen(false);
                onChoose(entry);
              }}
              role="menuitem"
              type="button"
            >
              <span className="cha-notes-highlight-excerpt">{entry.text.replace(/\s+/gu, ' ').trim()}</span>
            </button>
          ))}
        </div>
      )}
    </div>
  );
}
