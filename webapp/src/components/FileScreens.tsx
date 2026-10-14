import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type ChangeEvent,
  type FormEvent,
} from 'react';
import { ChaError, publicErrorMessage } from '../api/client';
import { DetailActions } from './DetailActions';
import {
  ChevronLeftIcon,
  ChevronRightIcon,
  FileUpIcon,
  PlusIcon,
  TextLinesIcon,
} from './Icons';
import {
  RosterDetailScreen,
  rosterMarkdown,
  RosterRow,
  type RosterDetailProps,
} from './RosterScreens';

export function FileList({ filenames, writable, onNew, onSelect }: {
  filenames: string[];
  writable: boolean;
  onNew(): void;
  onSelect(filename: string): void;
}) {
  return (
    <div className="cha-roster">
      {writable && (
        <button
          className="cha-list-action"
          onClick={onNew}
          type="button"
        >
          <span className="cha-list-icon"><PlusIcon /></span>
          <span className="cha-list-copy">
            <span className="cha-primary-line">New file</span>
          </span>
          <ChevronRightIcon className="cha-chevron" />
        </button>
      )}
      {filenames.map((filename) => (
        <RosterRow
          displayName={filename}
          key={filename}
          onSelect={filename.endsWith('.md') ? () => onSelect(filename) : undefined}
        />
      ))}
      {filenames.length === 0 && (
        <p className="cha-state-message">No files.</p>
      )}
    </div>
  );
}

export function CharacterFileScreen({
  state,
  navigate,
  client,
}: RosterDetailProps) {
  const filename = state.inspectedCharacter.file;
  const load = useCallback((characterId: string) => (
    client.getCharacterFile(characterId, filename!)
  ), [client, filename]);
  const characterId = state.inspectedCharacter.id;
  const characterName = state.bootstrap?.characters.find(
    ({ id }) => id === characterId,
  )?.display_name;
  return (
    <RosterDetailScreen
      key={`${characterId}/${filename}`}
      ariaLabel="Character file navigation"
      backLabel={characterName ?? 'Character'}
      copy={{
        absent: 'No file is selected.',
        loading: 'Loading file…',
        failed: 'Character file could not be loaded.',
      }}
      load={load}
      render={(file, update) => <>
        {file.writable && <div className="cha-detail-actions">
          <DetailActions
            name={filename!} subject="File"
            deleteMessage={`Delete “${filename}”? This permanently removes this Markdown file. This cannot be undone.`}
            onDelete={async () => {
              await client.deleteCharacterFile(characterId!, filename!);
              navigate({ type: 'inspect-character', characterId: characterId! });
            }}
            editor={{ title: 'Edit character file', value: file.content,
              uploadLabel: 'Replace character file content from file',
              onSave: async (content) => {
                update(await client.updateCharacterFile(characterId!, filename!, content));
              },
            }}
          />
        </div>}
        {rosterMarkdown(file.content, 'This file is empty.')}
      </>}
      onBack={() => navigate({ type: 'inspect-character', characterId: characterId! })}
      subjectId={filename ? characterId : null}
    />
  );
}

export function NewCharacterFileScreen(props: RosterDetailProps) {
  return <NewMarkdownFileScreen {...props} kind="character" />;
}

export function NewForumFileScreen(props: RosterDetailProps) {
  return <NewMarkdownFileScreen {...props} kind="forum" />;
}

function normalizeMarkdownFilename(filename: string) {
  const trimmed = filename.trim();
  return trimmed.endsWith('.md')
    ? trimmed
    : trimmed.replace(/(.+)\.[^.]*$/, '$1') + '.md';
}

function NewMarkdownFileScreen({
  kind,
  state,
  navigate,
  client,
}: RosterDetailProps & { kind: 'character' | 'forum' }) {
  const [mode, setMode] = useState<'upload' | 'text'>('upload');
  const [filename, setFilename] = useState('');
  const [content, setContent] = useState('');
  const [picture, setPicture] = useState<{ file: File; preview: string } | null>(null);
  const [pictureSaved, setPictureSaved] = useState(false);
  const [saving, setSaving] = useState(false);
  const [reading, setReading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const input = useRef<HTMLInputElement>(null);
  const mounted = useRef(false);
  const subjectId = (kind === 'character' ? state.inspectedCharacter.id : state.currentForumId)!;
  const subjectName = (kind === 'character' ? state.bootstrap?.characters : state.bootstrap?.forums)
    ?.find(({ id }) => id === subjectId)?.display_name;
  const pictureFilename = picture
    ? 'PICTURE' + picture.file.name.slice(picture.file.name.lastIndexOf('.')).toLowerCase()
    : '';
  const busy = saving || reading;
  const textWritable = kind === 'forum' || state.inspectedCharacter.writable;

  useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; };
  }, []);

  function back() {
    navigate(kind === 'character'
      ? { type: 'inspect-character', characterId: subjectId } : { type: 'show-forum-detail' });
  }

  async function upload(event: ChangeEvent<HTMLInputElement>) {
    const file = event.target.files?.[0];
    event.target.value = '';
    if (!file || busy) return;
    setError(null);
    setReading(true);
    try {
      if (/\.(png|jpe?g|webp|gif)$/i.test(file.name)) {
        if (kind !== 'character') throw new ChaError('invalid_argument', 'Pictures can only be uploaded to a character.');
        if (file.size > 8 * 1024 * 1024) throw new ChaError('body_too_large', 'Choose a picture no larger than 8 MB.');
        const preview = await new Promise<string>((resolve, reject) => {
          const reader = new FileReader();
          reader.onload = () => resolve(String(reader.result));
          reader.onerror = () => reject(new ChaError('invalid_argument', 'The picture file could not be read.'));
          reader.readAsDataURL(file);
        });
        if (!mounted.current) return;
        setPicture({ file, preview });
        setPictureSaved(false);
        setMode('upload');
      } else {
        if (!textWritable || file.type.startsWith('image/')
          || (!/\.(md|txt)$/i.test(file.name) && !file.type.startsWith('text/'))) {
          throw new ChaError('invalid_argument', kind === 'character'
            ? 'Choose a PNG, JPEG, WebP, GIF, Markdown, or text file.'
            : 'Choose a Markdown or text file.');
        }
        const text = await file.text();
        if (!mounted.current) return;
        setContent(text);
        setFilename(filename || normalizeMarkdownFilename(file.name));
        setMode('text');
      }
    } catch (failure: unknown) {
      if (mounted.current) setError(publicErrorMessage(failure, 'The local file could not be read.'));
    } finally {
      if (mounted.current) setReading(false);
    }
  }

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (busy || (mode === 'upload' ? !picture || pictureSaved : !filename.trim())) return;
    setSaving(true);
    setError(null);
    try {
      if (mode === 'upload' && picture) {
        await client.updateCharacterPicture(subjectId, picture.file);
        if (mounted.current) setPictureSaved(true);
      } else {
        const normalizedFilename = normalizeMarkdownFilename(filename);
        setFilename(normalizedFilename);
        const file = await (kind === 'character'
          ? client.createCharacterFile(subjectId, normalizedFilename, content)
          : client.createForumFile(subjectId, normalizedFilename, content));
        if (mounted.current) {
          navigate(kind === 'character'
            ? { type: 'inspect-character-file', characterId: subjectId, filename: file.filename }
            : { type: 'inspect-forum-file', forumId: subjectId, filename: file.filename });
        }
      }
    } catch (failure: unknown) {
      if (mounted.current) setError(publicErrorMessage(failure, `${kind === 'character' ? 'Character' : 'Forum'} file could not be added.`));
    } finally {
      if (mounted.current) setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label={`New ${kind} file navigation`}>
      <button className="cha-back-row" onClick={back} type="button">
        <ChevronLeftIcon />
        <span>{subjectName ?? (kind === 'character' ? 'Character' : 'Forum')}</span>
      </button>
      <form className="cha-settings-form" onSubmit={(event) => void submit(event)}>
        <div className="cha-file-actions">
          <button className="cha-button cha-button-primary cha-file-upload-button" disabled={busy}
            onClick={() => input.current?.click()} type="button">
            <FileUpIcon />Upload file
          </button>
          {textWritable && <button className="cha-button" disabled={busy} onClick={() => { setMode('text'); setError(null); }} type="button">
            <TextLinesIcon />Write text
          </button>}
          <input accept={kind === 'character'
            ? '.png,.jpg,.jpeg,.webp,.gif' + (textWritable ? ',.md,.txt,text/markdown,text/plain' : '')
            : '.md,.txt,text/markdown,text/plain'}
            aria-label="File to upload" className="cha-file-input" disabled={busy}
            onChange={(event) => void upload(event)} ref={input} type="file" />
        </div>
        {mode === 'upload' && picture && <>
          <label>Vault filename<input className="cha-form-control" readOnly value={pictureFilename} /></label>
          <div className="cha-uploaded-picture">
            <img alt={`Preview of ${picture.file.name}`} src={picture.preview}
              onError={() => { setPicture(null); setError('The picture cannot be displayed. Choose another image.'); }} />
            <span>{picture.file.name}</span>
          </div>
          {pictureSaved && <p role="status">Added as {pictureFilename}</p>}
        </>}
        {mode === 'text' && <>
          <label>Filename<input className="cha-form-control" value={filename}
            onChange={(event) => setFilename(event.target.value)} disabled={busy} required /></label>
          <label>Content<textarea className="cha-form-control" value={content}
            onChange={(event) => setContent(event.target.value)} disabled={busy} rows={12} /></label>
        </>}
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-dialog-actions cha-file-actions">
          <button className="cha-button" disabled={busy} onClick={back} type="button">Cancel</button>
          <button className="cha-button cha-button-primary"
            disabled={busy || (mode === 'upload' ? !picture || pictureSaved : !filename.trim())}
            type="submit">{saving ? 'Adding…' : 'Add file'}</button>
        </div>
      </form>
    </section>
  );
}

export function ForumFileScreen({
  state, navigate, client,
}: RosterDetailProps) {
  const forumId = state.currentForumId;
  const filename = state.inspectedForum.file;
  const load = useCallback((forumId: string) => (
    client.getForumFile(forumId, filename!)
  ), [client, filename]);
  const forumName = state.bootstrap?.forums.find(({ id }) => id === state.currentForumId)?.display_name;
  return (
    <RosterDetailScreen
      key={`${forumId}/${filename}`}
      ariaLabel="Forum file navigation"
      backLabel={forumName ?? 'Forum'}
      copy={{
        absent: 'No file is selected.',
        loading: 'Loading file…',
        failed: 'Forum file could not be loaded.',
      }}
      load={load}
      render={(file, update) => <>
        {file.writable && <div className="cha-detail-actions">
          <DetailActions
            name={filename!} subject="File"
            deleteMessage={`Delete “${filename}”? This permanently removes this Markdown file. This cannot be undone.`}
            onDelete={async () => {
              await client.deleteForumFile(forumId!, filename!);
              navigate({ type: 'show-forum-detail' });
            }}
            editor={{ title: 'Edit forum file', value: file.content,
              uploadLabel: 'Replace forum file content from file',
              onSave: async (content) => {
                update(await client.updateForumFile(forumId!, filename!, content));
              },
            }}
          />
        </div>}
        {rosterMarkdown(file.content, 'This file is empty.')}
      </>}
      onBack={() => navigate({ type: 'show-forum-detail' })}
      subjectId={filename ? state.currentForumId : null}
    />
  );
}
