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
  type CharacterDetail,
  type ForumSummary,
  type SessionListing,
  type VoiceDetail,
} from '../api/client';
import {
  getTextToSpeechConfiguration,
  TextToSpeechError,
  TextToSpeechSession,
  type TextToSpeechVoice,
} from '../textToSpeech';
import { sessionOperationState, type AppAction, type AppState } from '../state/view';
import { Markdown } from './Markdown';
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

interface DiscoveryScreenProps {
  state: AppState;
  dispatch: Dispatch<AppAction>;
}

// Opening a session can be started from anywhere — a Recent entry, a session
// row, a new name — so every navigation screen shows the outcome in place
// rather than replacing itself. The node is built once by the router above.
interface NavigationScreenProps extends DiscoveryScreenProps {
  sessionReport: ReactNode;
}

// Personas and Characters are the same control twice over: a roster of rows
// showing a display name above its optional configured description, each row
// opening that entry's read-only Markdown detail. Only the roster, the endpoint
// behind a row, and the wording differ, so the pair below is written once.
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

interface RosterDetailCopy {
  absent: string;
  loading: string;
  failed: string;
  // A persona's PERSONA.md is optional, so a roster entry can legitimately
  // resolve to no Markdown at all. That is a configuration to report, not a
  // failure and not a blank screen.
  empty: string;
}

interface RosterDetailScreenProps {
  ariaLabel: string;
  backLabel: string;
  copy: RosterDetailCopy;
  // Stable across renders, so reading one entry does not restart itself.
  load(subjectId: string): Promise<string>;
  onBack(): void;
  reloadVersion?: number;
  report?: ReactNode;
  sessionReport: ReactNode;
  subjectId: string | null;
  // Facts the roster already knows, shown above the Markdown and while it is
  // still loading. A persona or character has none; a forum names its cast.
  subtitle?: ReactNode;
  toolbarAction?: ReactNode;
}

function RosterDetailScreen({
  ariaLabel,
  backLabel,
  copy,
  load,
  onBack,
  reloadVersion = 0,
  report,
  sessionReport,
  subjectId,
  subtitle,
  toolbarAction,
}: RosterDetailScreenProps) {
  const [markdown, setMarkdown] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [requestVersion, setRequestVersion] = useState(0);

  useEffect(() => {
    if (!subjectId) return;
    let current = true;
    setMarkdown(null);
    setError(null);
    void load(subjectId).then(
      (loaded) => {
        if (current) setMarkdown(loaded);
      },
      (failure: unknown) => {
        if (current) setError(publicErrorMessage(failure, copy.failed));
      },
    );
    return () => {
      current = false;
    };
  }, [copy.failed, load, reloadVersion, requestVersion, subjectId]);

  return (
    <section className="cha-screen cha-navigation" aria-label={ariaLabel}>
      <div className="cha-detail-toolbar">
        <button className="cha-back-row" onClick={onBack} type="button">
          <ChevronLeftIcon />
          <span>{backLabel}</span>
        </button>
        {toolbarAction}
      </div>
      {sessionReport}
      {report}
      {subtitle}
      {!subjectId && <p className="cha-state-message">{copy.absent}</p>}
      {subjectId && markdown === null && !error && (
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
      {markdown !== null && (markdown.trim() === ''
        ? <p className="cha-state-message">{copy.empty}</p>
        : <Markdown source={markdown} />)}
    </section>
  );
}

interface RosterDetailProps extends NavigationScreenProps {
  client: ChaClient;
  reloadVersion?: number;
}

export function PersonasScreen({ state, dispatch, sessionReport }: NavigationScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Personas navigation">
      {sessionReport}
      <div className="cha-roster">
        <button
          className="cha-list-action"
          onClick={() => dispatch({ type: 'show-new-persona' })}
          type="button"
        >
          <span className="cha-list-icon"><PlusIcon /></span>
          <span className="cha-list-copy">
            <span className="cha-primary-line">New persona</span>
            <span className="cha-secondary-line">Enter a name to begin</span>
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
  sessionReport,
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
      {sessionReport}
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

export function PersonaDetailScreen({
  state,
  dispatch,
  client,
  reloadVersion = 0,
  sessionReport,
}: RosterDetailProps) {
  const load = useCallback(
    (personaId: string) => client.getPersona(personaId).then((detail) => {
      dispatch({
        type: 'persona-detail-loaded',
        personaId,
        writable: detail.writable,
      });
      return detail.persona_markdown;
    }),
    [client, dispatch],
  );

  return (
    <RosterDetailScreen
      ariaLabel="Persona detail navigation"
      backLabel="Personas"
      copy={{
        absent: 'No persona is selected.',
        loading: 'Loading persona…',
        failed: 'Persona detail could not be loaded.',
        empty: 'This persona has no PERSONA.md description.',
      }}
      load={load}
      onBack={() => dispatch({ type: 'show-personas' })}
      reloadVersion={reloadVersion}
      sessionReport={sessionReport}
      subjectId={state.inspectedPersonaId}
    />
  );
}

export function CharactersScreen({ state, dispatch, sessionReport }: NavigationScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Characters navigation">
      {sessionReport}
      <div className="cha-roster">
        <button
          className="cha-list-action"
          onClick={() => dispatch({ type: 'show-new-character' })}
          type="button"
        >
          <span className="cha-list-icon"><PlusIcon /></span>
          <span className="cha-list-copy">
            <span className="cha-primary-line">New character</span>
            <span className="cha-secondary-line">Enter a name to begin</span>
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
  sessionReport,
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
      {sessionReport}
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
  reloadVersion = 0,
  sessionReport,
}: RosterDetailProps) {
  const load = useCallback(
    (characterId: string) => client.getCharacter(characterId).then((detail) => {
      dispatch({ type: 'character-detail-loaded', characterId, writable: detail.writable });
      return detail.character_markdown;
    }),
    [client, dispatch],
  );
  return (
    <RosterDetailScreen
      ariaLabel="Character detail navigation"
      backLabel="Characters"
      copy={{
        absent: 'No character is selected.',
        loading: 'Loading character…',
        failed: 'Character detail could not be loaded.',
        empty: 'This character has no definition yet.',
      }}
      load={load}
      onBack={() => dispatch({ type: 'show-characters' })}
      reloadVersion={reloadVersion}
      sessionReport={sessionReport}
      subjectId={state.inspectedCharacterId}
      toolbarAction={state.characterSettingsAvailable ? (
        <button
          className="cha-detail-link"
          onClick={() => dispatch({ type: 'show-character-settings' })}
          type="button"
        >
          <span>Settings</span>
          <ChevronRightIcon />
        </button>
      ) : undefined}
    />
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

function voiceForTest(voice: VoiceDetail): TextToSpeechVoice {
  const settings: TextToSpeechVoice['settings'] = {};
  if (voice.stability !== null) settings.stability = voice.stability;
  if (voice.similarity_boost !== null) settings.similarity_boost = voice.similarity_boost;
  if (voice.style !== null) settings.style = voice.style;
  if (voice.use_speaker_boost !== null) {
    settings.use_speaker_boost = voice.use_speaker_boost;
  }
  if (voice.speed !== null) settings.speed = voice.speed;
  return { elevenlabs_voice_id: voice.elevenlabs_voice_id, settings };
}

export function CharacterSettingsScreen({
  state,
  dispatch,
  client,
  sessionReport,
}: RosterDetailProps) {
  const characterId = state.inspectedCharacterId;
  const character = state.bootstrap?.characters.find(({ id }) => id === characterId);
  const [detail, setDetail] = useState<CharacterDetail | null>(null);
  const [provider, setProvider] = useState<string | null>(null);
  const [style, setStyle] = useState<string | null>(null);
  const [voice, setVoice] = useState<string | null>(null);
  const [reasoningEffort, setReasoningEffort] =
    useState<CharacterDetail['reasoning_effort']>(null);
  const [webSearch, setWebSearch] = useState<CharacterDetail['web_search']>(null);
  const [voiceTestText, setVoiceTestText] = useState(
    'The chief task in life is simply this: to identify and separate matters so that I can say clearly to myself which are externals not under my control.',
  );
  const [testingVoice, setTestingVoice] = useState(false);
  const [voiceTestError, setVoiceTestError] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [requestVersion, setRequestVersion] = useState(0);
  const voiceTest = useRef<TextToSpeechSession | null>(null);
  const speechConfiguration = getTextToSpeechConfiguration();

  useEffect(() => () => voiceTest.current?.stop(), []);

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
    stopVoiceTest();
    if (characterId) dispatch({ type: 'inspect-character', characterId });
    else dispatch({ type: 'show-characters' });
  }

  function stopVoiceTest() {
    voiceTest.current?.stop();
    voiceTest.current = null;
    setTestingVoice(false);
  }

  async function toggleVoiceTest() {
    if (testingVoice) return stopVoiceTest();
    if (!speechConfiguration || !voiceTestText.trim()) return;
    setVoiceTestError(null);
    let selectedVoice: TextToSpeechVoice | undefined;
    if (voice !== null) {
      try {
        const registered = (await client.listVoices()).find(({ id }) => id === voice);
        if (!registered) {
          setVoiceTestError('That voice is not available for testing.');
          return;
        }
        selectedVoice = voiceForTest(registered);
      } catch (failure: unknown) {
        setVoiceTestError(publicErrorMessage(
          failure,
          'Voice settings could not be loaded for testing.',
        ));
        return;
      }
    }
    const session = new TextToSpeechSession(
      speechConfiguration,
      selectedVoice,
      voiceTestText.trim(),
      () => {
        if (voiceTest.current === session) voiceTest.current = null;
        setTestingVoice(false);
      },
    );
    voiceTest.current = session;
    setTestingVoice(true);
    try {
      await session.play();
    } catch (failure: unknown) {
      if (voiceTest.current !== session) return;
      session.stop();
      voiceTest.current = null;
      setTestingVoice(false);
      setVoiceTestError(failure instanceof TextToSpeechError
        ? failure.message : 'Voice test could not be played.');
    }
  }

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!characterId || !detail || provider === null || saving) return;
    if (provider === detail.provider && style === detail.style
      && voice === detail.voice_id
      && reasoningEffort === detail.reasoning_effort
      && webSearch === detail.web_search) return;
    setSaving(true);
    setError(null);
    try {
      const saved = await client.updateCharacter(characterId, {
        provider,
        style,
        voice_id: voice,
        reasoning_effort: reasoningEffort,
        web_search: webSearch,
      });
      dispatch({ type: 'character-updated', character: saved });
      setDetail(saved);
      setProvider(saved.provider);
      setStyle(saved.style);
      setVoice(saved.voice_id);
      setReasoningEffort(saved.reasoning_effort);
      setWebSearch(saved.web_search);
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
      || webSearch !== detail.web_search);

  return (
    <section className="cha-screen cha-navigation" aria-label="Character settings">
      <button className="cha-back-row" onClick={closeSettings} type="button">
        <ChevronLeftIcon />
        <span>{character?.display_name ?? 'Character'}</span>
      </button>
      {sessionReport}
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
            <option value="low">Low</option>
            <option value="medium">Medium</option>
            <option value="high">High</option>
            <option value="xhigh">Extra high</option>
          </select>
          <label htmlFor="cha-character-web-search">Web search</label>
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
            onChange={(event) => {
              stopVoiceTest();
              setVoice(event.target.value === '' ? null : event.target.value);
            }}
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
          <textarea
            aria-label="Voice preview text"
            className={`cha-form-control cha-voice-preview-text cha-message-text${voiceClasses(selectedStyle?.appearance)}`}
            id="cha-character-voice-test"
            onChange={(event) => setVoiceTestText(event.target.value)}
            value={voiceTestText}
          />
          {voiceTestError && <p className="cha-error-message" role="alert">{voiceTestError}</p>}
          {speechConfiguration && <div className="cha-new-session-actions">
            <button
              className="cha-button cha-voice-preview-action"
              disabled={!testingVoice && !voiceTestText.trim()}
              onClick={() => void toggleVoiceTest()}
              type="button"
            >
              {testingVoice ? <><StopIcon /> Stop preview</> : <><SpeakerIcon /> Play preview</>}
            </button>
          </div>}
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

export function ForumsScreen({ state, dispatch, sessionReport }: NavigationScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Forums navigation">
      {sessionReport}
      <div className="cha-roster">
        <button
          className="cha-list-action"
          onClick={() => dispatch({ type: 'show-new-forum' })}
          type="button"
        >
          <span className="cha-list-icon"><PlusIcon /></span>
          <span className="cha-list-copy">
            <span className="cha-primary-line">New forum</span>
            <span className="cha-secondary-line">Enter a name to begin</span>
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
  sessionReport,
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
      {sessionReport}
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
  reloadVersion = 0,
  sessionReport,
}: RosterDetailProps) {
  const load = useCallback(
    (forumId: string) => client.getForum(forumId).then((detail) => {
      dispatch({ type: 'forum-detail-loaded', forumId, writable: detail.writable });
      return detail.forum_markdown;
    }),
    [client, dispatch],
  );
  const forum = state.bootstrap?.forums.find(({ id }) => id === state.currentForumId);
  return (
    <RosterDetailScreen
      ariaLabel="Forum detail navigation"
      backLabel="Sessions"
      copy={{
        absent: 'No forum is selected.',
        loading: 'Loading forum…',
        failed: 'Forum detail could not be loaded.',
        empty: 'This forum has no FORUM.md description.',
      }}
      load={load}
      onBack={() => dispatch({ type: 'show-sessions' })}
      reloadVersion={reloadVersion}
      sessionReport={sessionReport}
      subjectId={state.currentForumId}
      subtitle={forum && <ForumCast forum={forum} />}
      toolbarAction={state.forumEditingAvailable ? (
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

export function ForumMembersScreen({
  state,
  dispatch,
  client,
  sessionReport,
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
      {sessionReport}
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

// Opening or creating a session is reported by the screen the user is looking
// at, so the list stays on screen and a half-typed session name is not thrown
// away. Chat is the exception: there the operation is the whole screen.
export function SessionOperationReport({
  state,
  onRetrySession,
  onReturnToWelcome,
}: {
  state: AppState;
  onRetrySession(): void;
  onReturnToWelcome(): void;
}) {
  const { pending, failure } = sessionOperationState(state);
  if (pending) {
    return (
      <p className="cha-state-message" role="status">
        {state.sessionOperationMessage ?? 'Opening session…'}
      </p>
    );
  }
  if (!failure) return null;
  return (
    <div className="cha-state-message cha-error-message" role="alert">
      <p>{failure}</p>
      {state.sessionOperationRetryable && (
        <div className="cha-state-actions">
          <button className="cha-button cha-button-ghost" onClick={onRetrySession} type="button">
            Retry
          </button>
          <button className="cha-button cha-button-ghost" onClick={onReturnToWelcome} type="button">
            Return to Welcome
          </button>
        </div>
      )}
    </div>
  );
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
  sessionReport,
}: SessionsScreenProps) {
  const [sessions, setSessions] = useState<SessionListing[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [requestVersion, setRequestVersion] = useState(0);
  const forumId = state.currentForumId;
  const forum = state.bootstrap?.forums.find(({ id }) => id === forumId);
  // The forum bootstrap starts in is the built-in one, whose single session the
  // server synthesizes; it stores no forum of its own, so a create there fails
  // as not-found. Offering the action would only produce that error.
  const canCreateSessions = forumId !== state.bootstrap?.initial_forum_id;

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
      {sessionReport}
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
                <span className="cha-secondary-line">Enter a name to begin</span>
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
  sessionReport,
}: NewSessionScreenProps) {
  const [name, setName] = useState('');
  const trimmedName = name.trim();
  const { pending: sessionPending, failure: sessionFailure } = sessionOperationState(state);

  function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!state.currentForumId || !trimmedName || sessionPending) return;
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
      {sessionReport}
      <form className="cha-new-session" onSubmit={submit}>
        <TransliteratingInput
          autoComplete="off"
          autoFocus
          className="cha-form-control"
          disabled={sessionPending}
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
            disabled={!trimmedName || sessionPending || state.sessionOperationRetryable}
            type="submit"
          >
            Start session
          </button>
        </div>
      </form>
    </section>
  );
}
