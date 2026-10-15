import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type ChangeEvent,
  type FormEvent,
} from 'react';
import { ChaError, publicErrorMessage, type CharacterDetail } from '../api/client';
import { DetailActions, EditableTitle } from './DetailActions';
import {
  ChevronLeftIcon,
  ChevronRightIcon,
  FileUpIcon,
  PlusIcon,
  PictureIcon,
  TextLinesIcon,
} from './Icons';
import {
  RosterDetailScreen,
  rosterMarkdown,
  RosterRow,
  type RosterDetailProps,
} from './RosterScreens';

function isPictureFile(filename: string) {
  return /\.(png|jpe?g|webp|gif)$/i.test(filename);
}

export function CharacterFilesScreen({ state, dispatch, navigate, client }: RosterDetailProps) {
  const characterId = state.inspectedCharacter.id;
  const load = useCallback((id: string) => client.getCharacter(id), [client]);
  const onLoaded = useCallback((detail: CharacterDetail, id: string) => {
    dispatch({ type: 'character-detail-loaded', characterId: id,
      settingsWritable: detail.settings_writable, writable: detail.writable });
  }, [dispatch]);
  return <RosterDetailScreen
    ariaLabel="Character files navigation" backLabel="Character settings"
    copy={{ absent: 'No character is selected.', loading: 'Loading files…', failed: 'Character files could not be loaded.' }}
    load={load} onLoaded={onLoaded} subjectId={characterId}
    onBack={() => navigate({ type: 'show-character-settings' })}
    render={(detail) => {
      const filenames = detail.markdown_files.filter(filename =>
        filename !== 'CHARACTER.md' && filename !== 'PROFILE.md'
        && (filename.endsWith('.md') || filename.endsWith('.txt') || isPictureFile(filename)));
      return <div className="cha-roster">
        {detail.settings_writable && <button className="cha-list-action"
          onClick={() => navigate({ type: 'show-new-character-file' })} type="button">
          <span className="cha-list-icon"><PlusIcon /></span>
          <span className="cha-list-copy"><span className="cha-primary-line">New file</span></span>
          <ChevronRightIcon className="cha-chevron" />
        </button>}
        {filenames.map(filename => <button className="cha-list-action" key={filename}
          onClick={() => navigate({ type: 'inspect-character-file', characterId: detail.id, filename })} type="button">
          <span className="cha-list-icon">{isPictureFile(filename) ? <PictureIcon /> : <TextLinesIcon />}</span>
          <span className="cha-list-copy"><span className="cha-primary-line">{filename}</span></span>
          <ChevronRightIcon className="cha-chevron" />
        </button>)}
        {filenames.length === 0 && <p className="cha-state-message">No files.</p>}
      </div>;
    }}
  />;
}

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

export function CharacterFileScreen(props: RosterDetailProps) {
  return props.state.inspectedCharacter.file && isPictureFile(props.state.inspectedCharacter.file)
    ? <CharacterPictureFileScreen {...props} /> : <CharacterTextFileScreen {...props} />;
}

function CharacterFileName({ state, navigate, client, writable, disabled }: Pick<RosterDetailProps, 'state' | 'navigate' | 'client'> & {
  writable: boolean; disabled?: boolean;
}) {
  const characterId = state.inspectedCharacter.id;
  const filename = state.inspectedCharacter.file;
  return <div className="cha-file-name"><EditableTitle filename subject="File" id={characterId}
    name={filename ?? undefined} disabled={disabled}
    available={writable && filename !== 'CHARACTER.md' && filename !== 'PROFILE.md'}
    onSave={async newFilename => {
      await client.renameCharacterFile(characterId!, filename!, newFilename);
      navigate({ type: 'inspect-character-file', characterId: characterId!, filename: newFilename });
    }} /></div>;
}

function CharacterTextFileScreen({
  state,
  navigate,
  client,
}: RosterDetailProps) {
  const filename = state.inspectedCharacter.file;
  const load = useCallback((characterId: string) => (
    client.getCharacterFile(characterId, filename!)
  ), [client, filename]);
  const characterId = state.inspectedCharacter.id;
  return (
    <RosterDetailScreen
      key={`${characterId}/${filename}`}
      ariaLabel="Character file navigation"
      backLabel="Files"
      copy={{
        absent: 'No file is selected.',
        loading: 'Loading file…',
        failed: 'Character file could not be loaded.',
      }}
      load={load}
      render={(file, update) => <>
        <div className="cha-detail-actions cha-character-file-header">
          <CharacterFileName state={state} navigate={navigate} client={client} writable={file.writable} />
          {file.writable && <DetailActions
            name={filename!} subject="File"
            deleteMessage={`Delete “${filename}”? This permanently removes this file. This cannot be undone.`}
            onDelete={async () => {
              await client.deleteCharacterFile(characterId!, filename!);
              navigate({ type: 'show-character-files' });
            }}
            editor={{ title: 'Edit character file', value: file.content,
              uploadLabel: 'Replace character file content from file',
              onSave: async (content) => {
                update(await client.updateCharacterFile(characterId!, filename!, content));
              },
            }}
          />}
        </div>
        {filename?.endsWith('.txt') && file.content.trim()
          ? <div className="cha-markdown"><pre>{file.content}</pre></div>
          : rosterMarkdown(file.content, 'This file is empty.')}
      </>}
      onBack={() => navigate({ type: 'show-character-files' })}
      subjectId={filename ? characterId : null}
    />
  );
}

function CharacterPictureFileScreen({ state, navigate, client }: RosterDetailProps) {
  const characterId = state.inspectedCharacter.id;
  const filename = state.inspectedCharacter.file;
  const input = useRef<HTMLInputElement>(null);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const load = useCallback(async (id: string) => {
    const picture = await client.getCharacterPicture(id, filename!);
    if (!picture) throw new ChaError('not_found', 'That character picture was not found.');
    return picture;
  }, [client, filename]);
  return <RosterDetailScreen
    key={`${characterId}/${filename}`} ariaLabel="Character picture navigation" backLabel="Files"
    copy={{ absent: 'No picture is selected.', loading: 'Loading picture…', failed: 'Character picture could not be loaded.' }}
    load={load} subjectId={filename ? characterId : null}
    onBack={() => navigate({ type: 'show-character-files' })}
    render={(picture, update) => <>
      <div className="cha-detail-actions cha-character-file-header">
        <CharacterFileName state={state} navigate={navigate} client={client}
          writable={state.inspectedCharacter.settingsWritable} disabled={saving} />
        {state.inspectedCharacter.settingsWritable && <div className="cha-detail-action-buttons">
          <input ref={input} type="file" accept=".png,.jpg,.jpeg,.webp,.gif" className="cha-file-input"
            aria-label="Replacement picture file" disabled={saving}
            onChange={async event => {
              const file = event.target.files?.[0];
              event.target.value = '';
              if (!file || saving) return;
              setError(null);
              if (!isPictureFile(file.name)) { setError('Choose a PNG, JPEG, WebP, or GIF file.'); return; }
              setSaving(true);
              try {
                const extension = file.name.slice(file.name.lastIndexOf('.')).toLowerCase();
                const savedFilename = filename!.toLowerCase().endsWith(extension)
                  ? filename! : filename!.slice(0, filename!.lastIndexOf('.')) + extension;
                await client.updateCharacterPicture(characterId!,
                  new File([file], savedFilename, { type: file.type }), filename!);
                const saved = await client.getCharacterPicture(characterId!, savedFilename);
                if (!saved) throw new ChaError('not_found', 'That character picture was not found.');
                if (saved.filename === filename) update(saved);
                else navigate({ type: 'inspect-character-file', characterId: characterId!, filename: saved.filename });
              } catch (failure: unknown) {
                setError(publicErrorMessage(failure, 'The picture could not be replaced.'));
              } finally { setSaving(false); }
            }} />
          <button className="cha-compact-icon-action" aria-label="Replace picture from file" disabled={saving}
            onClick={() => input.current?.click()} type="button"><FileUpIcon /></button>
          <DetailActions name={filename!} subject="File"
            deleteMessage={`Delete “${filename}”? This permanently removes this picture. This cannot be undone.`}
            onDelete={async () => {
              await client.deleteCharacterFile(characterId!, filename!);
              navigate({ type: 'show-character-files' });
            }} />
        </div>}
      </div>
      {error && <p className="cha-error-message" role="alert">{error}</p>}
      <div className="cha-character-file-picture">
        <img alt={filename!} src={`data:${picture.mime_type};base64,${picture.content_base64}`}
          onError={() => setError('The picture cannot be displayed. Choose another image.')} />
      </div>
    </>}
  />;
}

export function NewCharacterFileScreen(props: RosterDetailProps) {
  return <NewMarkdownFileScreen {...props} kind="character" />;
}

export function NewForumFileScreen(props: RosterDetailProps) {
  return <NewMarkdownFileScreen {...props} kind="forum" />;
}

function normalizeMarkdownFilename(filename: string, allowText = false) {
  const trimmed = filename.trim();
  return trimmed.endsWith('.md') || (allowText && trimmed.endsWith('.txt'))
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
  const busy = saving || reading;
  const textWritable = kind === 'forum' || state.inspectedCharacter.writable;

  useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; };
  }, []);

  function back() {
    navigate(kind === 'character'
      ? { type: 'show-character-files' } : { type: 'show-forum-detail' });
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
        setFilename(current => current.trim() ? current : file.name);
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
        setFilename(current => current.trim() ? current : normalizeMarkdownFilename(file.name, kind === 'character'));
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
    if (busy || !filename.trim() || (mode === 'upload' && (!picture || pictureSaved))) return;
    setSaving(true);
    setError(null);
    try {
      if (mode === 'upload' && picture) {
        await client.updateCharacterPicture(subjectId,
          new File([picture.file], filename.trim(), { type: picture.file.type }));
        if (mounted.current) setPictureSaved(true);
      } else {
        const normalizedFilename = normalizeMarkdownFilename(filename, kind === 'character');
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
        <span>{kind === 'character' ? 'Files' : (subjectName ?? 'Forum')}</span>
      </button>
      <form className="cha-settings-form" onSubmit={(event) => void submit(event)}>
        <label>Filename<input className="cha-form-control" value={filename}
          onChange={(event) => { setFilename(event.target.value); setPictureSaved(false); }} disabled={busy} required /></label>
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
          <div className="cha-uploaded-picture">
            <img alt={`Preview of ${picture.file.name}`} src={picture.preview}
              onError={() => { setPicture(null); setError('The picture cannot be displayed. Choose another image.'); }} />
          </div>
          {pictureSaved && <p role="status">Added as {filename.trim()}</p>}
        </>}
        {mode === 'text' && <>
          <label>Content<textarea className="cha-form-control" value={content}
            onChange={(event) => setContent(event.target.value)} disabled={busy} rows={12} /></label>
        </>}
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-dialog-actions cha-file-actions">
          <button className="cha-button" disabled={busy} onClick={back} type="button">Cancel</button>
          <button className="cha-button cha-button-primary"
            disabled={busy || !filename.trim() || (mode === 'upload' && (!picture || pictureSaved))}
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
