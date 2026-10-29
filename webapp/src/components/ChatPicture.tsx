import {
  useEffect, useRef, useState,
  type Dispatch, type PointerEvent, type ReactNode,
} from 'react';
import type { ChaClient } from '../api/client';
import type { AppState, DataAction } from '../state/view';

type PictureChoice = { requestId: number | undefined; filename: string };

// The parent key remounts this for each vault, session, and character.
function Picture({ client, characterId, displayName, requestId, previousPictures }: {
  client: ChaClient; characterId: string; displayName: string;
  requestId: number | undefined; previousPictures: Map<string, PictureChoice>;
}) {
  const [selection, setSelection] = useState<(PictureChoice & { filenames: string[] }) | null>(null);
  // Build the data URL once: the chat renders again for every streamed token.
  const [source, setSource] = useState<string | null>(null);
  const [failed, setFailed] = useState(false);
  useEffect(() => {
    let current = true;
    async function load() {
      const character = await client.getCharacter(characterId);
      if (!current) return;
      const filenames = character.markdown_files.filter((name) => /\.(png|jpe?g|webp|gif)$/i.test(name));
      if (!filenames.length) {
        setSelection(null);
        setSource(null);
        setFailed(false);
        return;
      }
      const previous = previousPictures.get(characterId);
      const previousIndex = previous ? filenames.indexOf(previous.filename) : -1;
      // The panel remounts when it opens again; keep this response's picture.
      const filename = previous && previous.requestId === requestId
        ? previous.filename : filenames[(previousIndex + 1) % filenames.length];
      setSelection({ requestId, filename, filenames });
    }
    void load().catch(() => { if (current) setFailed(true); });
    return () => { current = false; };
  }, [client, characterId, requestId, previousPictures]);

  useEffect(() => {
    if (!selection || selection.requestId !== requestId) return;
    let current = true;
    void client.getCharacterPicture(characterId, selection.filename).then(
      (picture) => {
        if (!current) return;
        if (picture) previousPictures.set(characterId, { requestId, filename: picture.filename });
        // Reset here, not before the request, so a broken old image does not show again.
        setFailed(false);
        setSource(picture ? `data:${picture.mime_type};base64,${picture.content_base64}` : null);
      },
      () => { if (current) setFailed(true); },
    );
    return () => { current = false; };
  }, [client, characterId, requestId, previousPictures, selection]);
  const content = failed ? <span role="alert">Picture unavailable</span>
    : source && <img alt={displayName} src={source} onError={() => setFailed(true)} />;
  if (!selection || selection.filenames.length < 2 || !content) return content;
  return <button type="button" className="cha-picture-next" aria-label="Next character picture"
    onClick={() => setSelection((current) => {
      if (!current) return current;
      const index = current.filenames.indexOf(current.filename);
      return { ...current, filename: current.filenames[(index + 1) % current.filenames.length] };
    })}>
    {content}
  </button>;
}

export function ChatPictureLayout({ children, client, dispatch, state }: {
  children: ReactNode; client: ChaClient; dispatch: Dispatch<DataAction>; state: AppState;
}) {
  const area = useRef<HTMLDivElement>(null);
  const [availableWidth, setAvailableWidth] = useState(0);
  const [dragging, setDragging] = useState(false);
  const drag = useRef<{ pointerId: number; startX: number; startWidth: number } | null>(null);
  useEffect(() => {
    const element = area.current;
    if (!element || typeof ResizeObserver === 'undefined') return;
    const observer = new ResizeObserver(([entry]) => setAvailableWidth(entry.contentRect.width));
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  // CSS applies the same limit to the panel, so the first layout is already
  // final and the transcript's scroll to its end holds. The measured width
  // serves only the divider. The divider overlays the panel edge.
  const maximum = Math.max(0, availableWidth - 320);
  const width = state.pictureOpen ? Math.min(state.pictureWidth, maximum) : 0;
  const resizable = maximum > 128;
  const character = state.sessionSnapshot?.characters.find(({ id }) => id === state.pictureCharacterId);
  const pictureKey = JSON.stringify([
    state.bootstrap?.vault_name, state.activeConversation?.forumId,
    state.activeConversation?.sessionId, character?.id,
  ]);
  // The last selection per character. The chat screen remounts for each conversation.
  const [previousPictures] = useState(() => new Map<string, PictureChoice>());
  const snapshot = state.sessionSnapshot;
  const lastTurn = snapshot && [...snapshot.transcript].reverse().find((entry) => (
    entry.request_id !== undefined && (entry.kind === 'human'
      ? entry.addressed_to === character?.id
      : (entry.kind === 'character' || entry.kind === 'error') && entry.participant_id === character?.id)
  ));
  // The prompt retains the request ID even if generation stops before answering.
  const requestId = snapshot?.generation.character_id === character?.id
    ? snapshot?.generation.request_id : lastTurn?.request_id;
  function resize(requested: number) {
    if (resizable) dispatch({ type: 'resize-picture', width: Math.min(maximum, Math.max(128, requested)) });
  }
  function finish(event: PointerEvent<HTMLDivElement>) {
    if (drag.current?.pointerId !== event.pointerId) return;
    drag.current = null;
    setDragging(false);
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
  }
  useEffect(() => {
    if (!state.pictureOpen || !resizable) {
      drag.current = null;
      setDragging(false);
    }
  }, [state.pictureOpen, resizable]);

  return <div className={`cha-screen cha-chat${dragging ? ' is-resizing-picture' : ''}`} ref={area}>
    {children}
    {state.pictureOpen && <div
      className="cha-picture"
      style={{ width: `min(${state.pictureWidth}px, max(0px, 100% - 320px))` }}
    >
      {width > 0 && <div
        aria-label="Resize picture"
        aria-orientation="vertical"
        aria-valuemin={resizable ? 128 : 0}
        aria-valuemax={maximum}
        aria-valuenow={width}
        aria-disabled={!resizable}
        role="separator"
        tabIndex={resizable ? 0 : -1}
        className="cha-picture-resize"
        onKeyDown={(event) => {
          if (!resizable || (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight')) return;
          event.preventDefault();
          resize(width + (event.key === 'ArrowLeft' ? 16 : -16));
        }}
        onPointerDown={(event) => {
          if (!resizable || event.button !== 0) return;
          event.preventDefault();
          event.currentTarget.setPointerCapture(event.pointerId);
          drag.current = { pointerId: event.pointerId, startX: event.clientX, startWidth: width };
          setDragging(true);
        }}
        onPointerMove={(event) => {
          const current = drag.current;
          if (current?.pointerId === event.pointerId) resize(current.startWidth + current.startX - event.clientX);
        }}
        onPointerUp={finish}
        onPointerCancel={finish}
        onLostPointerCapture={finish}
      />}
      <div className="cha-picture-content">
        {character && <Picture key={pictureKey} client={client} characterId={character.id}
          displayName={character.display_name} requestId={requestId} previousPictures={previousPictures} />}
      </div>
    </div>}
  </div>;
}
