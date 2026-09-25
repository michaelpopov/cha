import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type Dispatch,
  type FormEvent,
  type ReactNode,
} from 'react';

import {
  publicErrorMessage,
  type ChaClient,
  type CharacterAppearance,
  type CharacterDetail,
  type ForumDetail,
  type ForumSummary,
  type PersonaDetail,
  type SessionListing,
} from '../api/client';
import {
  nativeSpeechFromClient,
  TextToSpeechError,
  TextToSpeechSession,
  speechVoice,
  type TextToSpeechVoice,
  useTextToSpeechConfiguration,
} from '../textToSpeech';
import { type AppAction, type AppState } from '../state/view';
import { Markdown } from './Markdown';
import { BackToSettings } from './Settings';
import { DetailActions, EditableTitle } from './DetailActions';
import { TransliteratingInput } from './TransliterationMode';
import { voiceClasses } from './characterAppearance';
import {
  ChevronLeftIcon,
  ChevronRightIcon,
  MessageIcon,
  PlusIcon,
  SpeakerIcon,
  StopIcon,
} from './Icons';

interface NavigationScreenProps {
  state: AppState;
  dispatch: Dispatch<AppAction>;
}

// Personas, characters, and character files use the same navigation rows.
function RosterRow({ description, displayName, onSelect }: {
  description?: string;
  displayName: string;
  onSelect(): void;
}) {
  return (
    <button className="cha-roster-row" onClick={onSelect} type="button">
      <span className="cha-roster-copy">
        <span className="cha-roster-name">{displayName}</span>
        {description && <span className="cha-roster-description">{description}</span>}
      </span>
      <ChevronRightIcon className="cha-chevron" />
    </button>
  );
}

function MarkdownFileList({ filenames, writable, onNew, onSelect }: {
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
          onSelect={() => onSelect(filename)}
        />
      ))}
      {filenames.length === 0 && (
        <p className="cha-state-message">No Markdown files.</p>
      )}
    </div>
  );
}

interface RosterDetailCopy {
  absent: string;
  loading: string;
  failed: string;
}

interface RosterDetailScreenProps<Value> {
  ariaLabel: string;
  backLabel: string;
  copy: RosterDetailCopy;
  fallbackTitle?: string;
  // Stable across renders, so reading one entry does not restart itself.
  load(subjectId: string): Promise<Value>;
  onLoaded?(value: Value, subjectId: string): void;
  render(value: Value, update: (value: Value) => void): ReactNode;
  onBack(): void;
  subjectId: string | null;
  // Facts the roster already knows, shown above the content and while it is
  // still loading. A persona or character has none; a forum names its cast.
  subtitle?: ReactNode;
  toolbarAction?: ReactNode;
}

function RosterDetailScreen<Value>({
  ariaLabel,
  backLabel,
  copy,
  fallbackTitle,
  load,
  onLoaded,
  onBack,
  render,
  subjectId,
  subtitle,
  toolbarAction,
}: RosterDetailScreenProps<Value>) {
  const [value, setValue] = useState<Value | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [requestVersion, setRequestVersion] = useState(0);

  useEffect(() => {
    if (!subjectId) return;
    let current = true;
    setValue(null);
    setError(null);
    void load(subjectId).then(
      (loaded) => {
        if (current) {
          setValue(loaded);
          onLoaded?.(loaded, subjectId);
        }
      },
      (failure: unknown) => {
        if (current) setError(publicErrorMessage(failure, copy.failed));
      },
    );
    return () => {
      current = false;
    };
  }, [copy.failed, load, onLoaded, requestVersion, subjectId]);

  return (
    <section className="cha-screen cha-navigation" aria-label={ariaLabel}>
      <div className="cha-detail-toolbar">
        <button className="cha-back-row" onClick={onBack} type="button">
          <ChevronLeftIcon />
          <span>{backLabel}</span>
        </button>
        {toolbarAction}
      </div>
      {subtitle}
      {value === null && fallbackTitle && (
        <div className="cha-detail-actions"><h1>{fallbackTitle}</h1></div>
      )}
      {!subjectId && <p className="cha-state-message">{copy.absent}</p>}
      {subjectId && value === null && !error && (
        <p className="cha-state-message" role="status">{copy.loading}</p>
      )}
      {error && (
        <div className="cha-state-message cha-error-message" role="alert">
          <p>{error}</p>
          <button
            className="cha-button cha-button-ghost"
            onClick={() => setRequestVersion((version) => version + 1)}
            type="button"
          >
            Try again
          </button>
        </div>
      )}
      {value !== null && render(value, setValue)}
    </section>
  );
}

function rosterMarkdown(markdown: string, empty: string): ReactNode {
  return markdown.trim() === ''
    ? <p className="cha-state-message">{empty}</p>
    : <Markdown source={markdown} />;
}

interface RosterDetailProps extends NavigationScreenProps {
  client: ChaClient;
}

export function PersonasScreen({ state, dispatch }: NavigationScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Personas navigation">
      <BackToSettings dispatch={dispatch} />
      <div className="cha-roster">
        <button
          className="cha-list-action"
          onClick={() => dispatch({ type: 'show-new-persona' })}
          type="button"
        >
          <span className="cha-list-icon"><PlusIcon /></span>
          <span className="cha-list-copy">
            <span className="cha-primary-line">New persona</span>
          </span>
          <ChevronRightIcon className="cha-chevron" />
        </button>
        {state.bootstrap?.personas.map((persona) => (
          <RosterRow
            description={persona.description}
            displayName={persona.display_name}
            key={persona.id}
            onSelect={() => dispatch({ type: 'inspect-persona', personaId: persona.id })}
          />
        ))}
      </div>
    </section>
  );
}

export function NewPersonaScreen({
  dispatch,
  client,
}: RosterDetailProps) {
  const [name, setName] = useState('');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const trimmedName = name.trim();

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!trimmedName || saving) return;
    setSaving(true);
    setError(null);
    try {
      const persona = await client.createPersona({ display_name: trimmedName });
      dispatch({ type: 'persona-created', persona });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The persona could not be created.'));
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="New persona navigation">
      <button
        className="cha-back-row"
        onClick={() => dispatch({ type: 'show-personas' })}
        type="button"
      >
        <ChevronLeftIcon />
        <span>Personas</span>
      </button>
      <form className="cha-new-persona" onSubmit={(event) => void submit(event)}>
        <TransliteratingInput
          autoComplete="off"
          autoFocus
          className="cha-form-control"
          disabled={saving}
          id="cha-persona-name"
          label="Persona name"
          onValueChange={setName}
          placeholder="e.g. Project manager"
          type="text"
          value={name}
        />
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-new-persona-actions">
          <button
            className="cha-button cha-button-ghost"
            disabled={saving}
            onClick={() => dispatch({ type: 'show-personas' })}
            type="button"
          >
            Cancel
          </button>
          <button
            className="cha-button cha-button-primary"
            disabled={!trimmedName || saving}
            type="submit"
          >
            Create persona
          </button>
        </div>
      </form>
    </section>
  );
}

export function PersonaDetailScreen({ state, dispatch, client }: RosterDetailProps) {
  const id = state.inspectedPersona.id;
  const load = useCallback((id: string) => client.getPersona(id), [client]);
  const onLoaded = useCallback((detail: PersonaDetail, personaId: string) => {
    dispatch({ type: 'persona-detail-loaded', personaId, writable: detail.writable });
  }, [dispatch]);

  return (
    <RosterDetailScreen
      key={id}
      ariaLabel="Persona detail navigation"
      backLabel="Personas"
      fallbackTitle={state.bootstrap?.personas.find((persona) => persona.id === id)?.display_name}
      copy={{
        absent: 'No persona is selected.',
        loading: 'Loading persona…',
        failed: 'Persona detail could not be loaded.',
      }}
      load={load}
      onLoaded={onLoaded}
      onBack={() => dispatch({ type: 'show-personas' })}
      subjectId={id}
      toolbarAction={state.inspectedPersona.writable ? (
        <button className="cha-detail-link" onClick={() => dispatch({ type: 'show-persona-settings' })} type="button">
          <span>Settings</span><ChevronRightIcon />
        </button>
      ) : undefined}
      render={(detail, update) => {
        async function saveMarkdown(persona_markdown: string) {
          const saved = await client.updatePersona(detail.id, { persona_markdown });
          update(saved);
          dispatch({ type: 'persona-updated', persona: saved });
        }
        return <>
          <div className="cha-detail-actions">
            <EditableTitle
              available={detail.writable} id={detail.id} name={detail.display_name} subject="Persona"
              onSave={async (display_name) => {
                const saved = await client.updatePersona(detail.id, { display_name });
                update(saved);
                dispatch({ type: 'persona-updated', persona: saved });
              }}
            />
            {detail.writable && <DetailActions
              name={detail.display_name} subject="Persona"
              deleteMessage={`Delete “${detail.display_name}”? This permanently removes its profile. This cannot be undone.`}
              onDelete={async () => {
                await client.deletePersona(detail.id);
                dispatch({ type: 'persona-deleted', personaId: detail.id });
              }}
              editor={{ title: 'Edit persona profile', value: detail.persona_markdown,
                uploadLabel: 'Replace persona description from file', onSave: saveMarkdown }}
            />}
          </div>
          {rosterMarkdown(detail.persona_markdown, 'This persona has no PERSONA.md description.')}
        </>;
      }}
    />
  );
}

export function CharactersScreen({ state, dispatch }: NavigationScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Characters navigation">
      <BackToSettings dispatch={dispatch} />
      <div className="cha-roster">
        <button
          className="cha-list-action"
          onClick={() => dispatch({ type: 'show-new-character' })}
          type="button"
        >
          <span className="cha-list-icon"><PlusIcon /></span>
          <span className="cha-list-copy">
            <span className="cha-primary-line">New character</span>
          </span>
          <ChevronRightIcon className="cha-chevron" />
        </button>
        {state.bootstrap?.characters.map((character) => (
          <RosterRow
            description={character.description}
            displayName={character.display_name}
            key={character.id}
            onSelect={() => dispatch({ type: 'inspect-character', characterId: character.id })}
          />
        ))}
      </div>
    </section>
  );
}

export function NewCharacterScreen({
  dispatch,
  client,
}: RosterDetailProps) {
  const [name, setName] = useState('');
  const [description, setDescription] = useState('');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const trimmedName = name.trim();
  const trimmedDescription = description.trim();

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!trimmedName || !trimmedDescription || saving) return;
    setSaving(true);
    setError(null);
    try {
      const character = await client.createCharacter({
        display_name: trimmedName,
        description: trimmedDescription,
      });
      dispatch({ type: 'character-created', character });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The character could not be created.'));
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="New character navigation">
      <button
        className="cha-back-row"
        onClick={() => dispatch({ type: 'show-characters' })}
        type="button"
      >
        <ChevronLeftIcon />
        <span>Characters</span>
      </button>
      <form className="cha-new-character" onSubmit={(event) => void submit(event)}>
        <TransliteratingInput
          autoComplete="off"
          autoFocus
          className="cha-form-control"
          disabled={saving}
          id="cha-character-name"
          label="Name"
          onValueChange={setName}
          placeholder="e.g. Cheburashka"
          type="text"
          value={name}
        />
        <TransliteratingInput
          autoComplete="off"
          className="cha-form-control"
          disabled={saving}
          id="cha-character-description"
          label="Description"
          onValueChange={setDescription}
          placeholder="A short description shown in the character list"
          type="text"
          value={description}
        />
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-new-character-actions">
          <button
            className="cha-button cha-button-ghost"
            disabled={saving}
            onClick={() => dispatch({ type: 'show-characters' })}
            type="button"
          >
            Cancel
          </button>
          <button
            className="cha-button cha-button-primary"
            disabled={!trimmedName || !trimmedDescription || saving}
            type="submit"
          >
            Create character
          </button>
        </div>
      </form>
    </section>
  );
}

export function CharacterDetailScreen({
  state,
  dispatch,
  client,
}: RosterDetailProps) {
  const characterId = state.inspectedCharacter.id;
  const load = useCallback((id: string) => client.getCharacter(id), [client]);
  const onLoaded = useCallback((detail: CharacterDetail, id: string) => {
    dispatch({
      type: 'character-detail-loaded',
      characterId: id,
      settingsWritable: detail.settings_writable,
      writable: detail.writable,
    });
  }, [dispatch]);

  return (
    <RosterDetailScreen
      key={characterId}
      ariaLabel="Character detail navigation"
      backLabel="Characters"
      fallbackTitle={state.bootstrap?.characters.find(({ id }) => id === characterId)?.display_name}
      copy={{
        absent: 'No character is selected.',
        loading: 'Loading character…',
        failed: 'Character detail could not be loaded.',
      }}
      load={load}
      onLoaded={onLoaded}
      onBack={() => dispatch({ type: 'show-characters' })}
      subjectId={characterId}
      toolbarAction={state.inspectedCharacter.settingsWritable ? (
        <button
          className="cha-detail-link"
          onClick={() => dispatch({ type: 'show-character-settings' })}
          type="button"
        >
          <span>Settings</span>
          <ChevronRightIcon />
        </button>
      ) : undefined}
      render={(detail, update) => <>
        <div className="cha-detail-actions">
          <EditableTitle
            available={detail.writable} id={detail.id} name={detail.display_name} subject="Character"
            onSave={async (display_name) => {
              const saved = await client.updateCharacterDefinition(detail.id, { display_name });
              update(saved);
              dispatch({ type: 'character-updated', character: saved });
            }}
          />
          {detail.writable && <DetailActions
            name={detail.display_name} subject="Character"
            deleteMessage={`Delete “${detail.display_name}”? This permanently removes its definition and settings. Existing chat transcripts are kept. This cannot be undone.`}
            onDelete={async () => {
              await client.deleteCharacter(detail.id);
              dispatch({ type: 'character-deleted', characterId: detail.id });
            }}
          />}
        </div>
        <MarkdownFileList
          filenames={detail.markdown_files}
          writable={detail.writable}
          onNew={() => dispatch({ type: 'show-new-character-file' })}
          onSelect={(filename) => dispatch({ type: 'inspect-character-file', characterId: characterId!, filename })}
        />
      </>}
    />
  );
}

export function CharacterFileScreen({
  state,
  dispatch,
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
              dispatch({ type: 'inspect-character', characterId: characterId! });
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
      onBack={() => dispatch({ type: 'inspect-character', characterId: characterId! })}
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
  dispatch,
  client,
}: RosterDetailProps & { kind: 'character' | 'forum' }) {
  const [filename, setFilename] = useState('');
  const [content, setContent] = useState('');
  const [saving, setSaving] = useState(false);
  const [reading, setReading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const mounted = useRef(false);
  const subjectId = (kind === 'character' ? state.inspectedCharacter.id : state.currentForumId)!;
  const subjectName = (kind === 'character' ? state.bootstrap?.characters : state.bootstrap?.forums)
    ?.find(({ id }) => id === subjectId)?.display_name;

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
    };
  }, []);

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (saving || reading || !filename.trim()) return;
    const normalizedFilename = normalizeMarkdownFilename(filename);
    setFilename(normalizedFilename);
    setSaving(true);
    setError(null);
    try {
      const file = await (kind === 'character'
        ? client.createCharacterFile(subjectId, normalizedFilename, content)
        : client.createForumFile(subjectId, normalizedFilename, content));
      if (mounted.current) {
        dispatch(kind === 'character'
          ? { type: 'inspect-character-file', characterId: subjectId, filename: file.filename }
          : { type: 'inspect-forum-file', forumId: subjectId, filename: file.filename });
      }
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, `${kind === 'character' ? 'Character' : 'Forum'} file could not be added.`));
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label={`New ${kind} file navigation`}>
      <button
        className="cha-back-row"
        onClick={() => dispatch(kind === 'character'
          ? { type: 'inspect-character', characterId: subjectId } : { type: 'show-forum-detail' })}
        type="button"
      >
        <ChevronLeftIcon />
        <span>{subjectName ?? (kind === 'character' ? 'Character' : 'Forum')}</span>
      </button>
      <form className="cha-settings-form" onSubmit={(event) => void submit(event)}>
        <label>
          Filename
          <input
            className="cha-form-control"
            value={filename}
            onChange={(event) => setFilename(event.target.value)}
            disabled={saving || reading}
            required
          />
        </label>
        <label>
          Content
          <textarea
            className="cha-form-control"
            value={content}
            onChange={(event) => setContent(event.target.value)}
            disabled={saving || reading}
            rows={12}
          />
        </label>
        <label>
          Upload content
          <input
            accept=".md,.txt,text/markdown,text/plain"
            type="file"
            disabled={saving || reading}
            onChange={async (event) => {
              const file = event.target.files?.[0];
              if (!file) return;
              setReading(true);
              setError(null);
              try {
                const text = await file.text();
                if (!mounted.current) return;
                setContent(text);
                if (!filename) {
                  setFilename(normalizeMarkdownFilename(file.name));
                }
              } catch {
                setError('The local file could not be read.');
              } finally {
                setReading(false);
              }
            }}
          />
        </label>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-dialog-actions">
          <button
            className="cha-button cha-button-primary"
            disabled={saving || reading || !filename.trim()}
            type="submit"
          >
            {saving ? 'Adding…' : 'Add file'}
          </button>
        </div>
      </form>
    </section>
  );
}

// The lists hold only names the workspace could resolve, so a saved name it no
// longer can would match no option and read as though nothing were set. It is
// still what the file says — and still what a save resubmits — so it is listed,
// marked, and left selectable.
function unresolvedOption(
  options: readonly { id: string; label: string }[],
  saved: string | null,
): { id: string; label: string } | null {
  if (saved === null || options.some(({ id }) => id === saved)) return null;
  return { id: saved, label: `${saved} (not available)` };
}

function VoicePreview({ client, voiceId, appearance }: {
  client: ChaClient;
  voiceId: string | null;
  appearance: CharacterAppearance | undefined;
}) {
  const [text, setText] = useState(
    'The chief task in life is simply this: to identify and separate matters so that I can say clearly to myself which are externals not under my control.',
  );
  const [playing, setPlaying] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const sessionRef = useRef<TextToSpeechSession | null>(null);
  const speechConfiguration = useTextToSpeechConfiguration(client);

  useEffect(() => {
    sessionRef.current?.stop();
    sessionRef.current = null;
    setPlaying(false);
    return () => sessionRef.current?.stop();
  }, [voiceId]);

  function stop() {
    sessionRef.current?.stop();
    sessionRef.current = null;
    setPlaying(false);
  }

  async function toggle() {
    if (playing) return stop();
    if (!speechConfiguration || !text.trim()) return;
    setError(null);
    let selectedVoice: TextToSpeechVoice | undefined;
    if (voiceId !== null) {
      try {
        const registered = (await client.listVoices()).find(({ id }) => id === voiceId);
        if (!registered) {
          setError('That voice is not available for testing.');
          return;
        }
        selectedVoice = speechVoice(registered);
      } catch (failure: unknown) {
        setError(publicErrorMessage(
          failure,
          'Voice settings could not be loaded for testing.',
        ));
        return;
      }
    }
    const session = new TextToSpeechSession(
      speechConfiguration,
      selectedVoice,
      text.trim(),
      () => {
        if (sessionRef.current === session) sessionRef.current = null;
        setPlaying(false);
      },
      undefined,
      undefined,
      undefined,
      nativeSpeechFromClient(client),
      undefined,
      { onError: (failure) => setError(failure.message) },
    );
    sessionRef.current = session;
    setPlaying(true);
    try {
      await session.play();
    } catch (failure: unknown) {
      if (sessionRef.current !== session) return;
      session.stop();
      sessionRef.current = null;
      setPlaying(false);
      setError(failure instanceof TextToSpeechError
        ? failure.message : 'Voice test could not be played.');
    }
  }

  return (
    <>
      <textarea
        aria-label="Voice preview text"
        className={`cha-form-control cha-voice-preview-text cha-message-text${voiceClasses(appearance)}`}
        onChange={(event) => setText(event.target.value)}
        value={text}
      />
      {error && <p className="cha-error-message" role="alert">{error}</p>}
      {speechConfiguration && <div className="cha-new-session-actions">
        <button
          className="cha-button cha-voice-preview-action"
          disabled={!playing && !text.trim()}
          onClick={() => void toggle()}
          type="button"
        >
          {playing ? <><StopIcon /> Stop preview</> : <><SpeakerIcon /> Play preview</>}
        </button>
      </div>}
    </>
  );
}

export function PersonaSettingsScreen({
  state,
  dispatch,
  client,
}: RosterDetailProps) {
  const personaId = state.inspectedPersona.id;
  const persona = state.bootstrap?.personas.find(({ id }) => id === personaId);
  const [detail, setDetail] = useState<PersonaDetail | null>(null);
  const [style, setStyle] = useState<string | null>(null);
  const [voice, setVoice] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [requestVersion, setRequestVersion] = useState(0);

  useEffect(() => {
    if (!personaId) return;
    let current = true;
    setDetail(null);
    setError(null);
    void client.getPersona(personaId).then(
      (loaded) => {
        if (!current) return;
        setDetail(loaded);
        setStyle(loaded.style);
        setVoice(loaded.voice_id);
      },
      (failure: unknown) => {
        if (current) {
          setError(publicErrorMessage(failure, 'Persona settings could not be loaded.'));
        }
      },
    );
    return () => {
      current = false;
    };
  }, [client, personaId, requestVersion]);

  function closeSettings() {
    if (personaId) dispatch({ type: 'inspect-persona', personaId });
    else dispatch({ type: 'show-personas' });
  }

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!personaId || !detail || saving) return;
    if (style === detail.style && voice === detail.voice_id) return;
    setSaving(true);
    setError(null);
    try {
      const saved = await client.updatePersona(personaId, {
        style,
        voice_id: voice,
      });
      dispatch({ type: 'persona-updated', persona: saved });
      setDetail(saved);
      setStyle(saved.style);
      setVoice(saved.voice_id);
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Persona settings could not be saved.'));
    } finally {
      setSaving(false);
    }
  }

  const selectedStyle = detail?.available_styles.find(({ id }) => id === style);
  const unresolvedStyle = detail && unresolvedOption(detail.available_styles, detail.style);
  const unresolvedVoice = detail && unresolvedOption(detail.available_voices, detail.voice_id);
  const dirty = detail !== null
    && (style !== detail.style || voice !== detail.voice_id);

  return (
    <section className="cha-screen cha-navigation" aria-label="Persona settings">
      <button className="cha-back-row" onClick={closeSettings} type="button">
        <ChevronLeftIcon />
        <span>{persona?.display_name ?? 'Persona'}</span>
      </button>
      {!personaId && <p className="cha-state-message">No persona is selected.</p>}
      {personaId && detail === null && !error && (
        <p className="cha-state-message" role="status">Loading persona settings…</p>
      )}
      {error && (
        <div className="cha-state-message cha-error-message" role="alert">
          <p>{error}</p>
          {detail === null && (
            <button
              className="cha-button cha-button-ghost"
              onClick={() => setRequestVersion((version) => version + 1)}
              type="button"
            >
              Try again
            </button>
          )}
        </div>
      )}
      {detail && (
        <form className="cha-new-session" onSubmit={(event) => void save(event)}>
          <label htmlFor="cha-persona-style">Style</label>
          <select
            className="cha-form-control"
            disabled={saving}
            id="cha-persona-style"
            onChange={(event) => setStyle(event.target.value === '' ? null : event.target.value)}
            value={style ?? ''}
          >
            <option value="">No style</option>
            {detail.available_styles.map((option) => (
              <option key={option.id} value={option.id}>{option.label}</option>
            ))}
            {unresolvedStyle && (
              <option value={unresolvedStyle.id}>{unresolvedStyle.label}</option>
            )}
          </select>
          <label htmlFor="cha-persona-voice">Voice</label>
          <select
            className="cha-form-control"
            disabled={saving}
            id="cha-persona-voice"
            onChange={(event) => setVoice(event.target.value === '' ? null : event.target.value)}
            value={voice ?? ''}
          >
            <option value="">Application default</option>
            {detail.available_voices.map((option) => (
              <option key={option.id} value={option.id}>{option.label}</option>
            ))}
            {unresolvedVoice && (
              <option value={unresolvedVoice.id}>{unresolvedVoice.label}</option>
            )}
          </select>
          <VoicePreview
            appearance={selectedStyle?.appearance}
            client={client}
            voiceId={voice}
          />
          <div className="cha-new-session-actions">
            <button
              className="cha-button cha-button-ghost"
              onClick={closeSettings}
              type="button"
            >
              Cancel
            </button>
            <button
              className="cha-button cha-button-primary"
              disabled={!dirty || saving}
              type="submit"
            >
              Save
            </button>
          </div>
        </form>
      )}
    </section>
  );
}

export function CharacterSettingsScreen({
  state,
  dispatch,
  client,
}: RosterDetailProps) {
  const characterId = state.inspectedCharacter.id;
  const character = state.bootstrap?.characters.find(({ id }) => id === characterId);
  const [detail, setDetail] = useState<CharacterDetail | null>(null);
  const [provider, setProvider] = useState<string | null>(null);
  const [style, setStyle] = useState<string | null>(null);
  const [voice, setVoice] = useState<string | null>(null);
  const [reasoningEffort, setReasoningEffort] =
    useState<CharacterDetail['reasoning_effort']>(null);
  const [webSearch, setWebSearch] = useState<CharacterDetail['web_search']>(null);
  const [webSearchTool, setWebSearchTool] = useState<boolean | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [requestVersion, setRequestVersion] = useState(0);

  useEffect(() => {
    if (!characterId) return;
    let current = true;
    setDetail(null);
    setError(null);
    void client.getCharacter(characterId).then(
      (loaded) => {
        if (!current) return;
        setDetail(loaded);
        setProvider(loaded.provider);
        setStyle(loaded.style);
        setVoice(loaded.voice_id);
        setReasoningEffort(loaded.reasoning_effort);
        setWebSearch(loaded.web_search);
        setWebSearchTool(loaded.web_search_tool);
      },
      (failure: unknown) => {
        if (current) {
          setError(publicErrorMessage(failure, 'Character settings could not be loaded.'));
        }
      },
    );
    return () => {
      current = false;
    };
  }, [characterId, client, requestVersion]);

  function closeSettings() {
    if (characterId) dispatch({ type: 'inspect-character', characterId });
    else dispatch({ type: 'show-characters' });
  }

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!characterId || !detail || provider === null || saving) return;
    if (provider === detail.provider && style === detail.style
      && voice === detail.voice_id
      && reasoningEffort === detail.reasoning_effort
      && webSearch === detail.web_search
      && webSearchTool === detail.web_search_tool) return;
    setSaving(true);
    setError(null);
    try {
      const saved = await client.updateCharacter(characterId, {
        provider,
        style,
        voice_id: voice,
        reasoning_effort: reasoningEffort,
        web_search: webSearch,
        web_search_tool: webSearchTool,
      });
      dispatch({ type: 'character-updated', character: saved });
      setDetail(saved);
      setProvider(saved.provider);
      setStyle(saved.style);
      setVoice(saved.voice_id);
      setReasoningEffort(saved.reasoning_effort);
      setWebSearch(saved.web_search);
      setWebSearchTool(saved.web_search_tool);
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Character settings could not be saved.'));
    } finally {
      setSaving(false);
    }
  }

  const selectedStyle = detail?.available_styles.find(({ id }) => id === style);
  const unresolvedProvider = detail
    && unresolvedOption(detail.available_providers, detail.provider);
  const unresolvedStyle = detail
    && unresolvedOption(detail.available_styles, detail.style);
  const unresolvedVoice = detail
    && unresolvedOption(detail.available_voices, detail.voice_id);
  const dirty = detail !== null
    && (provider !== detail.provider || style !== detail.style
      || voice !== detail.voice_id
      || reasoningEffort !== detail.reasoning_effort
      || webSearch !== detail.web_search
      || webSearchTool !== detail.web_search_tool);

  return (
    <section className="cha-screen cha-navigation" aria-label="Character settings">
      <button className="cha-back-row" onClick={closeSettings} type="button">
        <ChevronLeftIcon />
        <span>{character?.display_name ?? 'Character'}</span>
      </button>
      {!characterId && <p className="cha-state-message">No character is selected.</p>}
      {characterId && detail === null && !error && (
        <p className="cha-state-message" role="status">Loading character settings…</p>
      )}
      {error && (
        <div className="cha-state-message cha-error-message" role="alert">
          <p>{error}</p>
          {detail === null && (
            <button
              className="cha-button cha-button-ghost"
              onClick={() => setRequestVersion((version) => version + 1)}
              type="button"
            >
              Try again
            </button>
          )}
        </div>
      )}
      {detail && (
        <form className="cha-new-session" onSubmit={(event) => void save(event)}>
          <label htmlFor="cha-character-provider">Provider</label>
          <select
            className="cha-form-control"
            disabled={saving}
            id="cha-character-provider"
            onChange={(event) => {
              setProvider(event.target.value);
            }}
            value={provider ?? ''}
          >
            <option disabled value="">Select provider</option>
            {detail.available_providers.map((option) => (
              <option key={option.id} value={option.id}>{option.label}</option>
            ))}
            {unresolvedProvider && (
              <option value={unresolvedProvider.id}>{unresolvedProvider.label}</option>
            )}
          </select>
          <label htmlFor="cha-character-reasoning-effort">Reasoning effort</label>
          <select
            className="cha-form-control"
            disabled={saving}
            id="cha-character-reasoning-effort"
            onChange={(event) => {
              setReasoningEffort(event.target.value === ''
                ? null
                : event.target.value as NonNullable<CharacterDetail['reasoning_effort']>);
            }}
            value={reasoningEffort ?? ''}
          >
            <option value="">Provider default</option>
            <option value="none">None</option>
            <option value="minimal">Minimal</option>
            <option value="low">Low</option>
            <option value="medium">Medium</option>
            <option value="high">High</option>
            <option value="xhigh">Extra high</option>
          </select>
          <label htmlFor="cha-character-web-search">Provider web search</label>
          <select
            className="cha-form-control"
            disabled={saving}
            id="cha-character-web-search"
            onChange={(event) => {
              setWebSearch(event.target.value === ''
                ? null
                : event.target.value as NonNullable<CharacterDetail['web_search']>);
            }}
            value={webSearch ?? ''}
          >
            <option value="">Provider default</option>
            <option value="off">Off</option>
            <option value="auto">Automatic</option>
            <option value="required">Required</option>
          </select>
          <label htmlFor="cha-character-web-search-tool">On-demand web search</label>
          <select className="cha-form-control" disabled={saving}
            id="cha-character-web-search-tool" value={webSearchTool === null ? '' : String(webSearchTool)}
            onChange={(event) => setWebSearchTool(event.target.value === '' ? null : event.target.value === 'true')}>
            <option value="">Workspace default</option>
            <option value="true">On</option>
            <option value="false">Off</option>
          </select>
          <label htmlFor="cha-character-style">Style</label>
          <select
            className="cha-form-control"
            disabled={saving}
            id="cha-character-style"
            onChange={(event) => {
              setStyle(event.target.value === '' ? null : event.target.value);
            }}
            value={style ?? ''}
          >
            <option value="">No style</option>
            {detail.available_styles.map((option) => (
              <option key={option.id} value={option.id}>{option.label}</option>
            ))}
            {unresolvedStyle && (
              <option value={unresolvedStyle.id}>{unresolvedStyle.label}</option>
            )}
          </select>
          <label htmlFor="cha-character-voice">Voice</label>
          <select
            className="cha-form-control"
            disabled={saving}
            id="cha-character-voice"
            onChange={(event) => setVoice(event.target.value === '' ? null : event.target.value)}
            value={voice ?? ''}
          >
            <option value="">Application default</option>
            {detail.available_voices.map((option) => (
              <option key={option.id} value={option.id}>{option.label}</option>
            ))}
            {unresolvedVoice && (
              <option value={unresolvedVoice.id}>{unresolvedVoice.label}</option>
            )}
          </select>
          <VoicePreview
            appearance={selectedStyle?.appearance}
            client={client}
            voiceId={voice}
          />
          <div className="cha-new-session-actions">
            <button
              className="cha-button cha-button-ghost"
              onClick={closeSettings}
              type="button"
            >
              Cancel
            </button>
            <button
              className="cha-button cha-button-primary"
              disabled={!dirty || provider === null || saving}
              type="submit"
            >
              Save
            </button>
          </div>
        </form>
      )}
    </section>
  );
}

export function forumMemberNames(forum: ForumSummary): string {
  return forum.members.map(({ display_name }) => display_name).join(', ') || 'No characters';
}

// Who is in a forum describes it as well as prose does, so a forum that
// configures no short description keeps naming its cast rather than showing a
// bare row. Either way the line answers the same question the other rosters
// answer with a description.
export function forumRosterDescription(forum: ForumSummary): string {
  return forum.description ?? forumMemberNames(forum);
}

export function ForumsScreen({ state, dispatch }: NavigationScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Forums navigation">
      <BackToSettings dispatch={dispatch} />
      <div className="cha-roster">
        <button
          className="cha-list-action"
          onClick={() => dispatch({ type: 'show-new-forum' })}
          type="button"
        >
          <span className="cha-list-icon"><PlusIcon /></span>
          <span className="cha-list-copy">
            <span className="cha-primary-line">New forum</span>
          </span>
          <ChevronRightIcon className="cha-chevron" />
        </button>
        {state.bootstrap?.forums.map((forum) => (
          <RosterRow
            description={forumRosterDescription(forum)}
            displayName={forum.display_name}
            key={forum.id}
            onSelect={() => dispatch({ type: 'select-forum', forumId: forum.id })}
          />
        ))}
      </div>
    </section>
  );
}

export function NewForumScreen({
  state,
  dispatch,
  client,
}: RosterDetailProps) {
  const personas = state.bootstrap?.personas ?? [];
  const [name, setName] = useState('');
  const [personaId, setPersonaId] = useState(personas[0]?.id ?? '');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const trimmedName = name.trim();

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!trimmedName || !personaId || saving) return;
    setSaving(true);
    setError(null);
    try {
      const forum = await client.createForum({
        display_name: trimmedName,
        persona_id: personaId,
      });
      dispatch({ type: 'forum-created', forum });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The forum could not be created.'));
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="New forum navigation">
      <button
        className="cha-back-row"
        onClick={() => dispatch({ type: 'show-forums' })}
        type="button"
      >
        <ChevronLeftIcon />
        <span>Forums</span>
      </button>
      <form className="cha-new-forum" onSubmit={(event) => void submit(event)}>
        <TransliteratingInput
          autoComplete="off"
          autoFocus
          className="cha-form-control"
          disabled={saving}
          id="cha-forum-name"
          label="Name"
          onValueChange={setName}
          placeholder="e.g. Brain Trust"
          type="text"
          value={name}
        />
        <label htmlFor="cha-forum-persona">Persona</label>
        <select
          className="cha-form-control"
          disabled={saving}
          id="cha-forum-persona"
          onChange={(event) => setPersonaId(event.target.value)}
          value={personaId}
        >
          {personas.map((persona) => (
            <option key={persona.id} value={persona.id}>{persona.display_name}</option>
          ))}
        </select>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-new-forum-actions">
          <button
            className="cha-button cha-button-ghost"
            disabled={saving}
            onClick={() => dispatch({ type: 'show-forums' })}
            type="button"
          >
            Cancel
          </button>
          <button
            className="cha-button cha-button-primary"
            disabled={!trimmedName || !personaId || saving}
            type="submit"
          >
            Create forum
          </button>
        </div>
      </form>
    </section>
  );
}

// Members and the persona a forum speaks as, shown under its name. Both are
// already in bootstrap, so this needs no request of its own.
function ForumCast({ forum }: { forum: ForumSummary }) {
  return (
    <p className="cha-forum-cast">
      {forumMemberNames(forum)}
      {' · speaking as '}
      {forum.default_persona_display_name}
    </p>
  );
}

export function ForumDetailScreen({
  state,
  dispatch,
  client,
  onDelete,
}: RosterDetailProps & { onDelete(forumId: string): Promise<void> }) {
  const load = useCallback((forumId: string) => client.getForum(forumId), [client]);
  const onLoaded = useCallback((detail: ForumDetail, forumId: string) => {
    dispatch({ type: 'forum-detail-loaded', forumId, writable: detail.writable });
  }, [dispatch]);
  const forum = state.bootstrap?.forums.find(({ id }) => id === state.currentForumId);
  return (
    <RosterDetailScreen
      key={state.currentForumId}
      ariaLabel="Forum detail navigation"
      backLabel="Sessions"
      fallbackTitle={forum?.display_name}
      copy={{
        absent: 'No forum is selected.',
        loading: 'Loading forum…',
        failed: 'Forum detail could not be loaded.',
      }}
      load={load}
      onLoaded={onLoaded}
      render={(detail, update) => <>
        <div className="cha-detail-actions">
          <EditableTitle
            available={detail.writable} id={detail.id} name={detail.display_name} subject="Forum"
            onSave={async (display_name) => {
              const saved = await client.updateForum(detail.id, { display_name });
              update(saved);
              dispatch({ type: 'forum-updated', forum: saved });
            }}
          />
          {detail.writable && <DetailActions
            name={detail.display_name} subject="Forum"
            deleteMessage={`Delete “${detail.display_name}”? This permanently removes the forum and all of its sessions. This cannot be undone.`}
            onDelete={() => onDelete(detail.id)}
          />}
        </div>
        <MarkdownFileList
          filenames={detail.markdown_files}
          writable={detail.writable}
          onNew={() => dispatch({ type: 'show-new-forum-file' })}
          onSelect={(filename) => dispatch({ type: 'inspect-forum-file', forumId: state.currentForumId!, filename })}
        />
      </>}
      onBack={() => dispatch({ type: 'show-sessions' })}
      subjectId={state.currentForumId}
      subtitle={forum && <ForumCast forum={forum} />}
      toolbarAction={state.inspectedForum.writable ? (
        <button
          className="cha-detail-link"
          onClick={() => dispatch({ type: 'show-forum-members' })}
          type="button"
        >
          <span>Members</span>
          <ChevronRightIcon />
        </button>
      ) : undefined}
    />
  );
}

export function ForumFileScreen({
  state, dispatch, client,
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
              dispatch({ type: 'show-forum-detail' });
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
      onBack={() => dispatch({ type: 'show-forum-detail' })}
      subjectId={filename ? state.currentForumId : null}
    />
  );
}

export function ForumMembersScreen({
  state,
  dispatch,
  client,
}: RosterDetailProps) {
  const forumId = state.currentForumId;
  const forum = state.bootstrap?.forums.find(({ id }) => id === forumId);
  const available = state.bootstrap?.characters ?? [];
  const personas = state.bootstrap?.personas ?? [];
  const memberKey = forum?.members.map(({ id }) => id).sort().join('\0') ?? '';
  const [selected, setSelected] = useState<Set<string>>(
    () => new Set(forum?.members.map(({ id }) => id)),
  );
  const [personaId, setPersonaId] = useState(forum?.default_persona_id ?? '');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setSelected(new Set(forum?.members.map(({ id }) => id)));
    setPersonaId(forum?.default_persona_id ?? '');
    setError(null);
  }, [forumId, memberKey, forum?.default_persona_id]);

  const dirty = forum !== undefined && (
    selected.size !== forum.members.length
    || forum.members.some(({ id }) => !selected.has(id))
    || personaId !== forum.default_persona_id
  );

  function toggle(characterId: string) {
    setSelected((current) => {
      const next = new Set(current);
      if (next.has(characterId)) next.delete(characterId);
      else next.add(characterId);
      return next;
    });
    setError(null);
  }

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!forumId || !dirty || selected.size === 0 || !personaId || saving) return;
    setSaving(true);
    setError(null);
    try {
      const updated = await client.updateForumMembers(forumId, {
        character_ids: [...selected],
        persona_id: personaId,
      });
      dispatch({ type: 'forum-updated', forum: updated });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Forum members could not be saved.'));
    } finally {
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="Forum members navigation">
      <button
        className="cha-back-row"
        onClick={() => dispatch({ type: 'show-forum-detail' })}
        type="button"
      >
        <ChevronLeftIcon />
        <span>{forum?.display_name ?? 'Forum'}</span>
      </button>
      {!forum && <p className="cha-state-message">No forum is selected.</p>}
      {forum && (
        <form className="cha-forum-members" onSubmit={(event) => void save(event)}>
          <select
            aria-label="Persona"
            className="cha-form-control"
            disabled={saving}
            id="cha-forum-members-persona"
            onChange={(event) => {
              setPersonaId(event.target.value);
              setError(null);
            }}
            value={personaId}
          >
            {personas.map((persona) => (
              <option key={persona.id} value={persona.id}>{persona.display_name}</option>
            ))}
          </select>
          <div className="cha-member-list">
            {available.map((character) => (
              <label className="cha-member-row" key={character.id}>
                <input
                  checked={selected.has(character.id)}
                  disabled={saving}
                  onChange={() => toggle(character.id)}
                  type="checkbox"
                />
                <span>{character.display_name}</span>
              </label>
            ))}
          </div>
          {error && <p className="cha-error-message" role="alert">{error}</p>}
          <div className="cha-forum-members-actions">
            <button
              className="cha-button cha-button-primary"
              disabled={!dirty || selected.size === 0 || !personaId || saving}
              type="submit"
            >
              Save
            </button>
          </div>
        </form>
      )}
    </section>
  );
}

interface SessionsScreenProps extends NavigationScreenProps {
  client: ChaClient;
  catalogRevision: number;
  onOpenSession(forumId: string, sessionId: string): Promise<boolean>;
}

export function formatSessionTime(updatedAt: number, now = Date.now()): string {
  const elapsed = Math.max(0, Math.floor(now / 1000) - updatedAt);
  if (elapsed < 60) return 'Now';
  if (elapsed < 60 * 60) return `${Math.floor(elapsed / 60)}m`;
  if (elapsed < 24 * 60 * 60) return `${Math.floor(elapsed / (60 * 60))}h`;
  if (elapsed < 7 * 24 * 60 * 60) return `${Math.floor(elapsed / (24 * 60 * 60))}d`;

  const date = new Date(updatedAt * 1000);
  const current = new Date(now);
  return new Intl.DateTimeFormat(undefined, {
    month: 'short',
    day: 'numeric',
    ...(date.getFullYear() === current.getFullYear() ? {} : { year: 'numeric' }),
  }).format(date);
}

export function SessionsScreen({
  state,
  dispatch,
  client,
  catalogRevision,
  onOpenSession,
}: SessionsScreenProps) {
  const [sessions, setSessions] = useState<SessionListing[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [requestVersion, setRequestVersion] = useState(0);
  const forumId = state.currentForumId;
  const forum = state.bootstrap?.forums.find(({ id }) => id === forumId);
  // Entrance has only the server's built-in Welcome session.
  const canCreateSessions = forumId !== state.bootstrap?.entrance_forum_id;

  useEffect(() => {
    if (!forumId) return;
    let current = true;
    setSessions(null);
    setError(null);
    void client.listSessions(forumId).then(
      (loaded) => {
        // The listing is not ordered by the server.
        if (current) setSessions([...loaded].sort((left, right) => right.updated_at - left.updated_at));
      },
      (failure: unknown) => {
        if (current) {
          setError(publicErrorMessage(failure, 'Sessions could not be loaded.'));
        }
      },
    );
    return () => {
      current = false;
    };
  }, [catalogRevision, client, forumId, requestVersion]);

  return (
    <section className="cha-screen cha-navigation" aria-label="Forum sessions navigation">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-forums' })} type="button">
        <ChevronLeftIcon />
        <span>All forums</span>
      </button>
      {/* The screen is titled Sessions, so without this the forum being listed
          is named nowhere once the sidebar is collapsed. It reads as a pushable
          row because that is what it is: the way into the forum's description. */}
      {forum && (
        <button
          className="cha-header-row"
          onClick={() => dispatch({ type: 'show-forum-detail' })}
          type="button"
        >
          <span className="cha-list-copy">
            <span className="cha-primary-line">{forum.display_name}</span>
            <span className="cha-secondary-line">{forumMemberNames(forum)}</span>
          </span>
          <ChevronRightIcon className="cha-chevron" />
        </button>
      )}
      {!forumId && <p className="cha-state-message">No forum is selected.</p>}
      {forumId && !sessions && !error && (
        <p className="cha-state-message" role="status">Loading sessions…</p>
      )}
      {forumId && error && (
        <div className="cha-state-message cha-error-message" role="alert">
          <p>{error}</p>
          <button
            className="cha-button cha-button-ghost"
            onClick={() => setRequestVersion((version) => version + 1)}
            type="button"
          >
            Try again
          </button>
        </div>
      )}
      {forumId && sessions?.length === 0 && (
        <p className="cha-state-message">
          {canCreateSessions
            ? 'No sessions in this forum yet. Create the first one below.'
            : 'This forum has no sessions.'}
        </p>
      )}
      {forumId && sessions && (
        <div className="cha-list">
          {canCreateSessions && (
            <button
              className="cha-list-action"
              onClick={() => dispatch({ type: 'show-new-session' })}
              type="button"
            >
              <span className="cha-list-icon"><PlusIcon /></span>
              <span className="cha-list-copy">
                <span className="cha-primary-line">New session</span>
              </span>
              <ChevronRightIcon className="cha-chevron" />
            </button>
          )}
          {sessions.map((session) => {
            const active = state.activeConversation?.forumId === forumId
              && state.activeConversation.sessionId === session.id;
            return (
              <button
                aria-current={active ? 'page' : undefined}
                className={`cha-list-action ${active ? 'is-current' : ''}`}
                key={session.id}
                onClick={() => void onOpenSession(forumId, session.id)}
                type="button"
              >
                <span className="cha-list-icon"><MessageIcon /></span>
                <span className="cha-list-copy">
                  <span className="cha-primary-line">{session.label}</span>
                </span>
                <time className="cha-secondary-line" dateTime={new Date(session.updated_at * 1000).toISOString()}>
                  {formatSessionTime(session.updated_at)}
                </time>
              </button>
            );
          })}
        </div>
      )}
    </section>
  );
}

interface NewSessionScreenProps extends NavigationScreenProps {
  onCreateSession(forumId: string, label: string): Promise<boolean>;
}

export function NewSessionScreen({
  state,
  dispatch,
  onCreateSession,
}: NewSessionScreenProps) {
  const [name, setName] = useState('');
  const trimmedName = name.trim();

  function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!state.currentForumId || !trimmedName) return;
    void onCreateSession(state.currentForumId, trimmedName);
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="New session navigation">
      <button
        className="cha-back-row"
        onClick={() => dispatch({ type: 'show-sessions' })}
        type="button"
      >
        <ChevronLeftIcon />
        <span>Sessions</span>
      </button>
      <form className="cha-new-session" onSubmit={submit}>
        <TransliteratingInput
          autoComplete="off"
          autoFocus
          className="cha-form-control"
          id="cha-session-name"
          label="Session name"
          onValueChange={setName}
          placeholder="e.g. Architecture review"
          type="text"
          value={name}
        />
        <div className="cha-new-session-actions">
          <button
            className="cha-button cha-button-ghost"
            onClick={() => dispatch({ type: 'show-sessions' })}
            type="button"
          >
            Cancel
          </button>
          <button
            className="cha-button cha-button-primary"
            disabled={!trimmedName}
            type="submit"
          >
            Start session
          </button>
        </div>
      </form>
    </section>
  );
}
