import { memo, useLayoutEffect, useMemo } from 'react';

import { renderNoteDocument } from './noteDocument';
import type { useChawebNotes } from './useChawebNotes';

const ArticleBody = memo(function ArticleBody({ html }: { html: string }) {
  return <div className="chaweb-note-body" dangerouslySetInnerHTML={{ __html: html }}
    onErrorCapture={(event) => {
      const image = event.target;
      if (image instanceof HTMLImageElement) {
        const fallback = document.createElement('span');
        fallback.textContent = image.alt.trim() || 'Image unavailable';
        image.replaceWith(fallback);
      }
    }} />;
});

export function NoteView({ model }: { model: ReturnType<typeof useChawebNotes> }) {
  const { note, items } = model;
  const rendered = useMemo(() => note ? renderNoteDocument(note.document, note.item, items ?? []) : null,
    [note, items]);
  useLayoutEffect(() => {
    const title = document.title;
    if (note) document.title = `${note.item.name} — CHA`;
    (document.scrollingElement ?? document.documentElement).scrollTop = 0;
    return () => { document.title = title; };
  }, [note]);

  return <main className="chaweb-note-main">
    {model.noteError ? <>
      <p className="chaweb-alert" role="alert">{model.noteError}</p>
      {!model.missing && <button className="chaweb-new-session" type="button" onClick={model.retry}>Retry</button>}
    </> : !note ? <p className="chaweb-status" role="status">Loading</p>
      : !rendered ? <p className="chaweb-alert" role="alert">This note cannot be displayed.</p>
        : <article aria-labelledby="chaweb-note-title">
          <header><h1 id="chaweb-note-title">{note.item.name}</h1></header>
          {rendered.empty ? <p>This note is empty.</p> : <ArticleBody html={rendered.html} />}
        </article>}
  </main>;
}
