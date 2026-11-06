import { useEffect, useLayoutEffect, useRef, type MouseEvent, type RefObject } from 'react';

import { CheckIcon, ChevronRightIcon } from '../components/Icons';
import { noteHash, notesHash } from './route';
import type { useChawebNotes } from './useChawebNotes';

export function followLocalLink(event: MouseEvent<HTMLAnchorElement>, navigate: () => void) {
  if (event.button !== 0 || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return;
  event.preventDefault();
  navigate();
}

export function NotesView({ model, listRef, navigate }: {
  model: ReturnType<typeof useChawebNotes>;
  listRef: RefObject<HTMLUListElement | null>;
  navigate(hash: string): void;
}) {
  const picker = useRef<HTMLDetailsElement>(null);
  const selected = model.folders.find((folder) => folder.id === model.folder);
  const loaded = model.items !== null;
  useEffect(() => {
    const close = (event: Event) => {
      if (event.target instanceof Node && !picker.current?.contains(event.target) && picker.current) {
        picker.current.open = false;
      }
    };
    document.addEventListener('pointerdown', close);
    document.addEventListener('focusin', close);
    return () => {
      document.removeEventListener('pointerdown', close);
      document.removeEventListener('focusin', close);
    };
  }, []);
  useEffect(() => {
    if (picker.current) picker.current.open = false;
  }, [model.folder]);
  // Restore once per list. A later metadata refresh must not move a list the user scrolls.
  useLayoutEffect(() => {
    const list = listRef.current;
    const offset: unknown = window.history.state?.chawebNotesScroll;
    if (list) list.scrollTop = typeof offset === 'number' && Number.isFinite(offset)
      ? Math.max(0, Math.min(offset, list.scrollHeight - list.clientHeight)) : 0;
  }, [model.folder, loaded, listRef]);

  return <div className="chaweb-sessions">
    {model.folders.length > 0 && <details className="chaweb-forum-picker" ref={picker}
      onKeyDown={(event) => {
        if (event.key === 'Escape') {
          event.preventDefault();
          event.currentTarget.open = false;
          event.currentTarget.querySelector('summary')?.focus();
        }
      }}>
      <summary className="chaweb-forum" aria-label="Folder" aria-describedby="chaweb-current-folder" role="button">
        <span className="chaweb-forum-text chaweb-forum-name" id="chaweb-current-folder">{selected?.name}</span>
        <ChevronRightIcon className="chaweb-forum-chevron" />
      </summary>
      <div aria-label="Folders" role="group" className="chaweb-forum-menu">
        {model.folders.map((folder) => <button type="button" className="chaweb-forum-option"
          key={folder.id} aria-pressed={folder.id === model.folder} onClick={() => {
            if (picker.current) {
              picker.current.open = false;
              picker.current.querySelector('summary')?.focus();
            }
            if (folder.id !== model.folder) navigate(notesHash(folder.id));
          }}>
          <span className="chaweb-forum-text chaweb-forum-name">{folder.name}</span><CheckIcon />
        </button>)}
      </div>
    </details>}
    {model.listLoading && !model.items && <p className="chaweb-status" role="status">Loading</p>}
    {model.listError && <>
      <p className="chaweb-alert" role="alert">{model.listError}</p>
      <button className="chaweb-new-session" type="button" onClick={model.retry}>Retry</button>
    </>}
    {model.items && (model.folders.length === 0
      ? <p className="chaweb-status">No notes.</p>
      : !selected && <p className="chaweb-alert" role="alert">That folder is no longer available.</p>)}
    <ul className="chaweb-list" aria-label="Notes" ref={listRef}>
      {model.rows.map((item) => <li key={item.id}>
        <a className="chaweb-session" href={noteHash(item.id)}
          onClick={(event) => followLocalLink(event, () => navigate(noteHash(item.id)))}>
          <span className="chaweb-session-title">{item.name}</span>
        </a>
      </li>)}
    </ul>
  </div>;
}
