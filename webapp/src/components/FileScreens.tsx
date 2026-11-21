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
  TextLinesIcon,
} from './Icons';
import {
  RosterDetailScreen,
  rosterMarkdown,
  RosterRow,
  type RosterDetailProps,
} from './RosterScreens';

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
        && (filename.endsWith('.md') || filename.endsWith('.txt')));
      return <div className="cha-roster">
        {detail.writable && <button className="cha-list-action"
          onClick={() => navigate({ type: 'show-new-character-file' })} type="button">
          <span className="cha-list-icon"><PlusIcon /></span>
          <span className="cha-list-copy"><span className="cha-primary-line">New file</span></span>
          <ChevronRightIcon className="cha-chevron" />
        </button>}
        {filenames.map(filename => <button className="cha-list-action" key={filename}
          onClick={() => navigate({ type: 'inspect-character-file', characterId: detail.id, filename })} type="button">
          <span className="cha-list-icon"><TextLinesIcon /></span>
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

function CharacterFileName({ state, navigate, client, writable }: Pick<RosterDetailProps, 'state' | 'navigate' | 'client'> & {
  writable: boolean;
}) {
  const characterId = state.inspectedCharacter.id;
  const filename = state.inspectedCharacter.file;
  return <div className="cha-file-name"><EditableTitle filename subject="File" id={characterId}
    name={filename ?? undefined}
    available={writable && filename !== 'CHARACTER.md' && filename !== 'PROFILE.md'}
    onSave={async newFilename => {
      await client.renameCharacterFile(characterId!, filename!, newFilename);
      navigate({ type: 'inspect-character-file', characterId: characterId!, filename: newFilename });
    }} /></div>;
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

export function NewCharacterFileScreen(props: RosterDetailProps) {
  return <NewMarkdownFileScreen key={props.state.inspectedCharacter.id} {...props} kind="character" />;
}

export function NewForumFileScreen(props: RosterDetailProps) {
  return <NewMarkdownFileScreen key={props.state.currentForumId} {...props} kind="forum" />;
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
  const [writing, setWriting] = useState(false);
  const [filename, setFilename] = useState('');
  const [content, setContent] = useState('');
  const [saving, setSaving] = useState(false);
  const [reading, setReading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const input = useRef<HTMLInputElement>(null);
  const mounted = useRef(false);
  const subjectId = (kind === 'character' ? state.inspectedCharacter.id : state.currentForumId)!;
  const subjectName = (kind === 'character' ? state.bootstrap?.characters : state.bootstrap?.forums)
    ?.find(({ id }) => id === subjectId)?.display_name;
  const busy = saving || reading;
  const canSubmit = writing && !!filename.trim();

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
      if (file.type.startsWith('image/')
        || (!/\.(md|txt)$/i.test(file.name) && !file.type.startsWith('text/'))) {
        throw new ChaError('invalid_argument', 'Choose a Markdown or text file.');
      }
      const text = await file.text();
      if (!mounted.current) return;
      setContent(text);
      setFilename(current => current.trim() ? current : normalizeMarkdownFilename(file.name, kind === 'character'));
      setWriting(true);
    } catch (failure: unknown) {
      if (mounted.current) setError(publicErrorMessage(failure, 'The local file could not be read.'));
    } finally {
      if (mounted.current) setReading(false);
    }
  }

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (busy || !canSubmit) return;
    setSaving(true);
    setError(null);
    try {
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
          onChange={(event) => setFilename(event.target.value)} disabled={busy} required /></label>
        <div className="cha-file-actions">
          <button className="cha-button cha-button-primary" disabled={busy}
            onClick={() => input.current?.click()} type="button">
            <FileUpIcon />Upload file
          </button>
          <button className="cha-button" disabled={busy} onClick={() => { setWriting(true); setError(null); }} type="button">
            <TextLinesIcon />Write text
          </button>
          <input accept=".md,.txt,text/markdown,text/plain"
            aria-label="File to upload" className="cha-file-input" disabled={busy}
            onChange={(event) => void upload(event)} ref={input} type="file" />
        </div>
        {writing && <>
          <label>Content<textarea className="cha-form-control" value={content}
            onChange={(event) => setContent(event.target.value)} disabled={busy} rows={12} /></label>
        </>}
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions">
          <button className="cha-button cha-button-ghost" disabled={busy} onClick={back} type="button">Cancel</button>
          <button className="cha-button cha-button-primary"
            disabled={busy || !canSubmit}
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
