import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type FormEvent,
} from 'react';
import {
  publicErrorMessage,
  type ChaClient,
  type CharacterAppearance,
  type CharacterDetail,
} from '../api/client';
import { StaleNotice, useFormReload } from '../detailRefresh';
import {
  nativeSpeechFromClient,
  speechVoice,
  TextToSpeechError,
  TextToSpeechSession,
  useTextToSpeechConfiguration,
  type TextToSpeechVoice,
} from '../textToSpeech';
import { voiceClasses } from './characterAppearance';
import { DetailActions, EditableTitle } from './DetailActions';
import {
  ChevronLeftIcon,
  ChevronRightIcon,
  PlusIcon,
  SpeakerIcon,
  StopIcon,
} from './Icons';
import { BackToSettings } from './SettingsShared';
import { TransliteratingInput } from './TransliterationMode';
import { FileList } from './FileScreens';
import {
  RosterDetailScreen,
  RosterRow,
  unresolvedOption,
  type NavigationScreenProps,
  type RosterDetailProps,
} from './RosterScreens';

export function CharactersScreen({ state, navigate }: NavigationScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Characters navigation">
      <BackToSettings navigate={navigate} />
      <div className="cha-roster">
        <button
          className="cha-list-action"
          onClick={() => navigate({ type: 'show-new-character' })}
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
            onSelect={() => navigate({ type: 'inspect-character', characterId: character.id })}
          />
        ))}
      </div>
    </section>
  );
}

export function NewCharacterScreen({
  navigate,
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
      navigate({ type: 'character-created', character });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The character could not be created.'));
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="New character navigation">
      <button
        className="cha-back-row"
        onClick={() => navigate({ type: 'show-characters' })}
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
            onClick={() => navigate({ type: 'show-characters' })}
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
  navigate,
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
      onBack={() => navigate({ type: 'show-characters' })}
      subjectId={characterId}
      toolbarAction={state.inspectedCharacter.settingsWritable ? (
        <button
          className="cha-detail-link"
          onClick={() => navigate({ type: 'show-character-settings' })}
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
              navigate({ type: 'character-deleted', characterId: detail.id });
            }}
          />}
        </div>
        <FileList
          filenames={detail.markdown_files}
          writable={detail.settings_writable}
          onNew={() => navigate({ type: 'show-new-character-file' })}
          onSelect={(filename) => navigate({ type: 'inspect-character-file', characterId: characterId!, filename })}
        />
      </>}
    />
  );
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

export function CharacterSettingsScreen({
  state,
  dispatch,
  navigate,
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
  const reload = useFormReload(characterId || null);

  function applyCharacter(loaded: CharacterDetail) {
    setDetail(loaded);
    setProvider(loaded.provider);
    setStyle(loaded.style);
    setVoice(loaded.voice_id);
    setReasoningEffort(loaded.reasoning_effort);
    setWebSearch(loaded.web_search);
    setWebSearchTool(loaded.web_search_tool);
  }

  useEffect(() => {
    if (!characterId) return;
    let current = true;
    const background = reload.start();
    if (!background) {
      setDetail(null);
      setError(null);
    }
    void client.getCharacter(characterId).then(
      (loaded) => {
        if (!current) return;
        reload.loaded(loaded, applyCharacter);
      },
      (failure: unknown) => {
        if (!current) return;
        setError(publicErrorMessage(failure, 'Character settings could not be loaded.'));
        reload.fail(background);
      },
    );
    return () => {
      current = false;
    };
  }, [characterId, client, reload.epoch, reload.attempt]);

  function closeSettings() {
    if (characterId) navigate({ type: 'inspect-character', characterId });
    else navigate({ type: 'show-characters' });
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
      applyCharacter(saved);
      reload.remember(saved);
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
  reload.markDirty(dirty);

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
          {(detail === null || reload.refreshFailed) && (
            <button
              className="cha-button cha-button-ghost"
              onClick={reload.retry}
              type="button"
            >
              Try again
            </button>
          )}
        </div>
      )}
      {detail && characterId && (
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
          {reload.stale && <StaleNotice onReload={() => reload.accept(applyCharacter)} />}
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
              disabled={!dirty || provider === null || saving || reload.blocked}
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
