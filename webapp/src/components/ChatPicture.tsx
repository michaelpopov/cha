import {
  useEffect, useRef, useState,
  type Dispatch, type PointerEvent, type ReactNode,
} from 'react';
import type { ChaClient } from '../api/client';
import type { AppAction, AppState } from '../state/view';

// The parent key remounts this for each vault, session, and character.
function Picture({ client, characterId, displayName }: {
  client: ChaClient; characterId: string; displayName: string;
}) {
  // Build the data URL once: the chat renders again for every streamed token.
  const [source, setSource] = useState<string | null>(null);
  const [failed, setFailed] = useState(false);
  useEffect(() => {
    let current = true;
    void client.getCharacterPicture(characterId).then(
      (picture) => {
        if (current && picture) setSource(`data:${picture.mime_type};base64,${picture.content_base64}`);
      },
      () => { if (current) setFailed(true); },
    );
    return () => { current = false; };
  }, [client, characterId]);
  if (failed) return <span role="alert">Picture unavailable</span>;
  return source && <img alt={displayName} src={source} onError={() => setFailed(true)} />;
}

export function ChatPictureLayout({ children, client, dispatch, state }: {
  children: ReactNode; client: ChaClient; dispatch: Dispatch<AppAction>; state: AppState;
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
        {character && <Picture key={pictureKey} client={client} characterId={character.id} displayName={character.display_name} />}
      </div>
    </div>}
  </div>;
}
