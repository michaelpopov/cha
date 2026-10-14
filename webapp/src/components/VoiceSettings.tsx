import {
  useEffect,
  useRef,
  useState,
  type FormEvent,
} from 'react';
import {
  publicErrorMessage,
  type ApiKeyDetail,
  type ChaClient,
  type VoiceDetail,
  type VoiceInputSettings,
  type VoiceOutputSettings,
  type VoiceUpdate,
} from '../api/client';
import { DetailRefreshProvider, StaleNotice, useFormReload } from '../detailRefresh';
import {
  nativeSpeechFromClient,
  speechVoice,
  TextToSpeechError,
  TextToSpeechSession,
  useTextToSpeechConfiguration,
} from '../textToSpeech';
import { useLoad } from '../useLoad';
import { ConfirmDialog } from './ConfirmDialog';
import { EditableTitle } from './DetailActions';
import {
  ChevronLeftIcon,
  ChevronRightIcon,
  PlusIcon,
  SpeakerIcon,
  StopIcon,
} from './Icons';
import { TransliteratingInput } from './TransliterationMode';
import {
  BackToSettings,
  LoadFailure,
  SettingsRow,
  UsedBy,
  type SettingsScreenProps,
} from './SettingsShared';

const loadVoices = (client: ChaClient) => client.listVoices();
const loadVoiceOutput = (client: ChaClient) => client.getVoiceOutputSettings();

export function VoicesScreen({ client, navigate }: SettingsScreenProps) {
  const { data: voices, error, retry } = useLoad(
    client, loadVoices, 'Voices could not be loaded.',
  );
  return (
    <section className="cha-screen cha-navigation" aria-label="Voices settings">
      <div className="cha-detail-toolbar">
        <BackToSettings navigate={navigate} />
        <button
          className="cha-detail-link"
          onClick={() => navigate({ type: 'show-settings-voice-input' })}
          type="button"
        >
          <span>Voice settings</span>
          <ChevronRightIcon />
        </button>
      </div>
      {voices === null && !error && <p className="cha-state-message" role="status">Loading voices…</p>}
      {error && <LoadFailure message={error} retry={retry} />}
      {voices && (
        <div className="cha-list">
          <SettingsRow description="Add a voice for character speech" icon={<PlusIcon />} label="New voice" onClick={() => navigate({ type: 'show-settings-new-voice' })} />
          {voices.length === 0 && <p className="cha-empty-list">No voices configured</p>}
          {voices.map((voice) => (
            <SettingsRow
              description={voice.description}
              icon={<SpeakerIcon />}
              key={voice.id}
              label={voice.display_name}
              onClick={() => navigate({
                type: 'inspect-voice',
                voiceId: voice.id,
                voiceName: voice.display_name,
              })}
            />
          ))}
        </div>
      )}
    </section>
  );
}

const openAiVoiceInputDefaults = {
  provider: 'openai' as const,
  url: 'https://api.openai.com/v1/realtime/calls',
  model: 'gpt-live-transcribe',
};

const xaiVoiceInputDefaults = {
  provider: 'xai' as const,
  url: 'wss://api.x.ai/v1/stt',
  model: 'grok-voice-transcribe-2.0',
};

const defaultVoiceInput: VoiceInputSettings = {
  ...openAiVoiceInputDefaults,
  api_key: '',
  delay: 'low',
  prompt: '',
  send_phrase: 'over to you',
};

const voiceInputAuthority = /^[A-Za-z0-9.\-:[\]]+$/;

function voiceInputUrlError(
  provider: VoiceInputSettings['provider'],
  url: string,
): string | null {
  const requirement = provider === 'xai'
    ? 'xAI voice input requires an absolute WS or WSS URL'
    : 'OpenAI voice input requires an absolute HTTP or HTTPS URL';
  const prefix = provider === 'xai'
    ? (url.startsWith('wss://') ? 'wss://' : url.startsWith('ws://') ? 'ws://' : '')
    : (url.startsWith('https://') ? 'https://' : url.startsWith('http://') ? 'http://' : '');
  if (!prefix) return requirement;
  const authority = url.slice(prefix.length).split(/[/?#]/, 1)[0] ?? '';
  if (!authority || !voiceInputAuthority.test(authority)) return requirement;
  return null;
}

function defaultVoiceOutput(voices: VoiceDetail[]): VoiceOutputSettings {
  return {
    url: 'https://api.fish.audio/v1/tts',
    model: 's2.1-pro',
    api_key: '',
    output_format: 'mp3',
    default_voice: voices[0]?.display_name ?? '',
  };
}

export function VoiceSettingsScreen({ client, navigate }: SettingsScreenProps) {
  const [savedInput, setSavedInput] =
    useState<VoiceInputSettings | null | undefined>(undefined);
  const [input, setInput] = useState<VoiceInputSettings>(defaultVoiceInput);
  const [savedOutput, setSavedOutput] =
    useState<VoiceOutputSettings | null | undefined>(undefined);
  const [output, setOutput] = useState<VoiceOutputSettings>(defaultVoiceOutput([]));
  const [outputProvider, setOutputProvider] = useState<'fishaudio' | 'elevenlabs'>('fishaudio');
  const elevenLabsDefaults: NonNullable<VoiceOutputSettings['elevenlabs']> = { url: 'https://api.elevenlabs.io/v1/text-to-speech',
    model: 'eleven_multilingual_v2', api_key: '', output_format: 'mp3_44100_128' };
  const providerOutput = outputProvider === 'elevenlabs'
    ? output.elevenlabs ?? elevenLabsDefaults : output;
  function changeOutput(field: 'url' | 'model' | 'api_key' | 'output_format' | 'connection', value: string) {
    setOutput((current) => outputProvider === 'elevenlabs'
      ? { ...current, elevenlabs: { ...(current.elevenlabs ?? elevenLabsDefaults), [field]: value } }
      : { ...current, [field]: value });
  }
  const [keys, setKeys] = useState<ApiKeyDetail[] | null>(null);
  const [voices, setVoices] = useState<VoiceDetail[] | null>(null);
  const [savingInput, setSavingInput] = useState(false);
  const [savingOutput, setSavingOutput] = useState(false);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [inputError, setInputError] = useState<string | null>(null);
  const [outputError, setOutputError] = useState<string | null>(null);
  const [inputMessage, setInputMessage] = useState<string | null>(null);
  const [outputMessage, setOutputMessage] = useState<string | null>(null);
  const reload = useFormReload('voice-settings');

  function applyVoiceSettings(value: {
    input: VoiceInputSettings | null;
    output: VoiceOutputSettings | null;
    keys: ApiKeyDetail[];
    voices: VoiceDetail[];
  }) {
    setSavedInput(value.input);
    setInput(value.input ?? defaultVoiceInput);
    setSavedOutput(value.output);
    setOutput(value.output ?? defaultVoiceOutput(value.voices));
    setOutputProvider(value.output?.api_key || !value.output?.elevenlabs?.api_key
      ? 'fishaudio' : 'elevenlabs');
    setKeys(value.keys);
    setVoices(value.voices);
  }

  useEffect(() => {
    let current = true;
    const background = reload.start();
    if (!background) {
      setSavedInput(undefined);
      setSavedOutput(undefined);
      setKeys(null);
      setVoices(null);
      setLoadError(null);
    }
    void Promise.all([
      client.getVoiceInputSettings(),
      client.getVoiceOutputSettings(),
      client.listApiKeys(),
      client.listVoices(),
    ]).then(
      ([inputSettings, outputSettings, loadedKeys, loadedVoices]) => {
        if (!current) return;
        setLoadError(null);
        reload.loaded({
          input: inputSettings,
          output: outputSettings,
          keys: loadedKeys,
          voices: loadedVoices,
        }, applyVoiceSettings);
      },
      (failure: unknown) => {
        if (!current) return;
        setLoadError(publicErrorMessage(failure, 'Voice settings could not be loaded.'));
        reload.fail(background);
      },
    );
    return () => { current = false; };
  }, [client, reload.epoch, reload.attempt]);

  const inputBaseline = savedInput ?? defaultVoiceInput;
  const outputBaseline = savedOutput ?? defaultVoiceOutput(voices ?? []);
  const inputDirty = input.provider !== inputBaseline.provider
    || input.url !== inputBaseline.url
    || input.model !== inputBaseline.model
    || input.api_key !== inputBaseline.api_key
    || input.delay !== inputBaseline.delay
    || input.prompt !== inputBaseline.prompt
    || input.send_phrase !== inputBaseline.send_phrase;
  const outputDirty = output.url !== outputBaseline.url
    || output.model !== outputBaseline.model
    || (output.connection ?? 'http') !== (outputBaseline.connection ?? 'http')
    || output.api_key !== outputBaseline.api_key
    || output.output_format !== outputBaseline.output_format
    || output.default_voice !== outputBaseline.default_voice
    || JSON.stringify(output.elevenlabs) !== JSON.stringify(outputBaseline.elevenlabs);
  const inputReady = Boolean(input.url.trim() && input.model.trim() && input.api_key);
  reload.markDirty(inputDirty || outputDirty);
  function resetVoiceInput() {
    setInputMessage(null);
    setInputError(null);
    if (reload.stale) {
      reload.accept(applyVoiceSettings);
      return;
    }
    setInput(inputBaseline);
  }
  function resetVoiceOutput() {
    setOutputMessage(null);
    setOutputError(null);
    if (reload.stale) {
      reload.accept(applyVoiceSettings);
      return;
    }
    setOutput(outputBaseline);
  }
  const outputReady = Boolean(
    ((output.url.trim() && output.model.trim() && output.api_key && output.output_format.trim())
      || (output.elevenlabs?.url.trim() && output.elevenlabs.model.trim()
        && output.elevenlabs.api_key && output.elevenlabs.output_format.trim()))
    && output.default_voice,
  );

  function chooseInputProvider(provider: VoiceInputSettings['provider']) {
    const defaults = provider === 'xai' ? xaiVoiceInputDefaults : openAiVoiceInputDefaults;
    setInput((current) => ({
      ...current,
      provider,
      url: defaults.url,
      model: defaults.model,
      api_key: '',
    }));
    setInputError(null);
    setInputMessage(null);
  }

  async function saveInput(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    const url = input.url.trim();
    const model = input.model.trim();
    if (!url || !model || !input.api_key || savingInput) return;
    const urlError = voiceInputUrlError(input.provider, url);
    if (urlError) {
      setInputMessage(null);
      setInputError(urlError);
      return;
    }
    setInputError(null);
    setInputMessage(null);
    setSavingInput(true);
    try {
      const updatedInput = await client.saveVoiceInputSettings({
        provider: input.provider,
        url,
        model,
        api_key: input.api_key,
        delay: input.delay,
        prompt: input.prompt,
        send_phrase: input.send_phrase.trim(),
      });
      setSavedInput(updatedInput);
      setInput(updatedInput);
      reload.remember({
        input: updatedInput,
        output: savedOutput ?? null,
        keys: keys ?? [],
        voices: voices ?? [],
      });
      setInputMessage('Voice input saved.');
    } catch (failure: unknown) {
      setInputError(publicErrorMessage(failure, 'Voice input could not be saved.'));
    } finally {
      setSavingInput(false);
    }
  }

  async function saveOutput(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!outputReady || savingOutput) return;
    setOutputError(null);
    setOutputMessage(null);
    setSavingOutput(true);
    try {
      const updatedOutput = await client.saveVoiceOutputSettings({
        url: output.url,
        model: output.model,
        ...(output.connection ? { connection: output.connection } : {}),
        api_key: output.api_key,
        output_format: output.output_format.trim(),
        default_voice: output.default_voice,
        ...(output.elevenlabs?.api_key ? { elevenlabs: {
          url: output.elevenlabs.url, model: output.elevenlabs.model,
          api_key: output.elevenlabs.api_key, output_format: output.elevenlabs.output_format,
          ...(output.elevenlabs.connection ? { connection: output.elevenlabs.connection } : {}),
        } } : {}),
      });
      setSavedOutput(updatedOutput);
      setOutput(updatedOutput);
      reload.remember({
        input: savedInput ?? null,
        output: updatedOutput,
        keys: keys ?? [],
        voices: voices ?? [],
      });
      setOutputMessage('Voice output saved.');
    } catch (failure: unknown) {
      setOutputError(publicErrorMessage(failure, 'Voice output could not be saved.'));
    } finally {
      setSavingOutput(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="Voice settings">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-voices' })} type="button"><ChevronLeftIcon /><span>Voices</span></button>
      {(savedInput === undefined || savedOutput === undefined) && !loadError && <p className="cha-state-message" role="status">Loading voice settings…</p>}
      {loadError && (savedInput === undefined || savedOutput === undefined || reload.refreshFailed) && <LoadFailure message={loadError} retry={reload.retry} />}
      {reload.stale && <StaleNotice onReload={() => reload.accept(applyVoiceSettings)} />}
      {savedInput !== undefined && savedOutput !== undefined && keys && voices && (
        <>
          {keys.length === 0 && <p className="cha-error-message" role="alert">Add an API key before configuring voice.</p>}
          <form className="cha-settings-form" onSubmit={(event) => void saveInput(event)}>
            <label>Input provider<select autoFocus className="cha-form-control" onChange={(event) => chooseInputProvider(event.target.value as VoiceInputSettings['provider'])} value={input.provider}><option value="openai">OpenAI</option><option value="xai">xAI</option></select></label>
            <label>Input URL endpoint<input className="cha-form-control" onChange={(event) => setInput({ ...input, url: event.target.value })} value={input.url} /></label>
            <label>Input model name<input className="cha-form-control" onChange={(event) => setInput({ ...input, model: event.target.value })} value={input.model} /></label>
            <label>Input API key name<select className="cha-form-control" onChange={(event) => setInput({ ...input, api_key: event.target.value })} value={input.api_key}><option value="">Select an API key</option>{keys.map((key) => <option key={key.id} value={key.id}>{key.display_name}</option>)}</select></label>
            <label>Hands-free send phrase<input className="cha-form-control" onChange={(event) => setInput({ ...input, send_phrase: event.target.value })} value={input.send_phrase} /></label>
            {input.provider === 'openai' && (
              <>
                <label>Input delay<select className="cha-form-control" onChange={(event) => setInput({ ...input, delay: event.target.value as VoiceInputSettings['delay'] })} value={input.delay}><option value="low">Low</option><option value="medium">Medium</option><option value="high">High</option><option value="xhigh">Extra high</option></select></label>
                <label>Input prompt<textarea className="cha-form-control" onChange={(event) => setInput({ ...input, prompt: event.target.value })} rows={4} value={input.prompt} /></label>
              </>
            )}
            {inputMessage && <p className="cha-state-message" role="status">{inputMessage}</p>}
            {inputError && <p className="cha-error-message" role="alert">{inputError}</p>}
            <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={(!inputDirty && !reload.stale) || savingInput} onClick={resetVoiceInput} type="button">Reset voice input</button><button className="cha-button cha-button-primary" disabled={!inputDirty || !inputReady || savingInput || reload.blocked} type="submit">{savingInput ? 'Saving…' : 'Save voice input'}</button></div>
          </form>
          <form className="cha-settings-form" onSubmit={(event) => void saveOutput(event)}>
            <label>Output provider<select className="cha-form-control" value={outputProvider} onChange={(event) => setOutputProvider(event.target.value as typeof outputProvider)}><option value="fishaudio">FishAudio</option><option value="elevenlabs">ElevenLabs</option></select></label>
            <label>Output URL endpoint<input className="cha-form-control" onChange={(event) => changeOutput('url', event.target.value)} type="url" value={providerOutput.url} /></label>
            <label>Output model name<input className="cha-form-control" onChange={(event) => changeOutput('model', event.target.value)} value={providerOutput.model} /></label>
            <label>Connection<select className="cha-form-control" value={providerOutput.connection ?? 'http'} onChange={(event) => changeOutput('connection', event.target.value)}><option value="http">HTTP</option><option value="websocket">WebSocket</option></select></label>
            <label>Output API key name<select className="cha-form-control" onChange={(event) => changeOutput('api_key', event.target.value)} value={providerOutput.api_key}><option value="">Select an API key</option>{keys.map((key) => <option key={key.id} value={key.id}>{key.display_name}</option>)}</select></label>
            <label>Output format<input className="cha-form-control" onChange={(event) => changeOutput('output_format', event.target.value)} value={providerOutput.output_format} /></label>
            <label>Default voice<select className="cha-form-control" onChange={(event) => setOutput({ ...output, default_voice: event.target.value })} value={output.default_voice}><option value="">Select a voice</option>{voices.map((voice) => <option key={voice.id} value={voice.display_name}>{voice.display_name}</option>)}</select></label>
            {voices.length === 0 && <p className="cha-error-message" role="alert">Add a voice before configuring voice output.</p>}
            {outputMessage && <p className="cha-state-message" role="status">{outputMessage}</p>}
            {outputError && <p className="cha-error-message" role="alert">{outputError}</p>}
            <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={(!outputDirty && !reload.stale) || savingOutput} onClick={resetVoiceOutput} type="button">Reset voice output</button><button className="cha-button cha-button-primary" disabled={!outputDirty || !outputReady || savingOutput || reload.blocked} type="submit">{savingOutput ? 'Saving…' : 'Save voice output'}</button></div>
          </form>
        </>
      )}
    </section>
  );
}

export function NewVoiceScreen({ client, navigate }: SettingsScreenProps) {
  const [name, setName] = useState('');
  const [description, setDescription] = useState('');
  const [voiceReferenceId, setVoiceReferenceId] = useState('');
  const [provider, setProvider] = useState<'fishaudio' | 'elevenlabs'>('fishaudio');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!name.trim() || !description.trim() || !voiceReferenceId.trim() || saving) return;
    setSaving(true);
    setError(null);
    try {
      const created = await client.createVoice({
        display_name: name.trim(),
        description: description.trim(),
        elevenlabs_voice_id: voiceReferenceId.trim(),
        provider,
      });
      navigate({
        type: 'inspect-voice',
        voiceId: created.id,
        voiceName: created.display_name,
      });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The voice could not be created.'));
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="New voice settings">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-voices' })} type="button"><ChevronLeftIcon /><span>Voices</span></button>
      <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
        <TransliteratingInput autoFocus className="cha-form-control" disabled={saving} id="cha-new-voice-name" label="Name" onValueChange={setName} placeholder="e.g. Brian" value={name} />
        <label htmlFor="cha-new-voice-description">Description<textarea className="cha-form-control cha-voice-description" disabled={saving} id="cha-new-voice-description" onChange={(event) => setDescription(event.target.value)} placeholder="Describe how this voice sounds" value={description} /></label>
        <label>Provider<select className="cha-form-control" disabled={saving} value={provider} onChange={(event) => setProvider(event.target.value as typeof provider)}><option value="fishaudio">FishAudio</option><option value="elevenlabs">ElevenLabs</option></select></label>
        <label htmlFor="cha-new-voice-reference-id">Voice ID<input className="cha-form-control" disabled={saving} id="cha-new-voice-reference-id" onChange={(event) => setVoiceReferenceId(event.target.value)} value={voiceReferenceId} /></label>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving} onClick={() => navigate({ type: 'show-settings-voices' })} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!name.trim() || !description.trim() || !voiceReferenceId.trim() || saving} type="submit">{saving ? 'Registering…' : 'Register voice'}</button></div>
      </form>
    </section>
  );
}

function voiceUpdate(detail: VoiceDetail): VoiceUpdate {
  const { id: _id, used_by: _usedBy, writable: _writable, ...update } = detail;
  return update;
}

function optionalNumber(value: string): number | null {
  return value === '' ? null : Number(value);
}

export function VoiceScreen({
  client,
  dispatch,
  navigate,
  state,
}: SettingsScreenProps) {
  const id = state.inspectedVoice.id;
  const [detail, setDetail] = useState<VoiceDetail | null>(null);
  const [draft, setDraft] = useState<VoiceUpdate | null>(null);
  const [previewText, setPreviewText] = useState(
    'The chief task in life is simply this: to identify and separate matters so that I can say clearly to myself which are externals not under my control.',
  );
  const [previewing, setPreviewing] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [previewError, setPreviewError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const reload = useFormReload(id);
  const preview = useRef<TextToSpeechSession | null>(null);
  const speechConfiguration = useTextToSpeechConfiguration(client);
  const { data: voiceOutput, error: voiceOutputError, retry: retryVoiceOutput } = useLoad(
    client, loadVoiceOutput, 'Voice output settings could not be loaded.',
  );

  function applyVoice(loaded: VoiceDetail) {
    setDetail(loaded);
    setDraft(voiceUpdate(loaded));
    dispatch({
      type: 'voice-detail-loaded',
      voiceId: loaded.id,
      voiceName: loaded.display_name,
      writable: loaded.writable,
    });
  }

  useEffect(() => () => preview.current?.stop(), []);
  useEffect(() => {
    let current = true;
    const background = reload.start();
    if (!background) {
      preview.current?.stop();
      preview.current = null;
      setPreviewing(false);
      setDetail(null);
      setDraft(null);
      setError(null);
    }
    if (!id) return () => { current = false; };
    void client.listVoices().then(
      (voices) => {
        if (!current) return;
        const loaded = voices.find((voice) => voice.id === id);
        if (!loaded) {
          setError('That voice was not found.');
          reload.fail(background);
          return;
        }
        setError(null);
        reload.loaded(loaded, applyVoice);
      },
      (failure: unknown) => {
        if (!current) return;
        setError(publicErrorMessage(failure, 'Voice settings could not be loaded.'));
        reload.fail(background);
      },
    );
    return () => { current = false; };
  }, [client, dispatch, id, reload.epoch, reload.attempt]);

  function change<Key extends keyof VoiceUpdate>(key: Key, value: VoiceUpdate[Key]) {
    setDraft((current) => current ? { ...current, [key]: value } : current);
  }

  function stopPreview() {
    preview.current?.stop();
    preview.current = null;
    setPreviewing(false);
  }

  async function togglePreview() {
    if (previewing) return stopPreview();
    if (!draft || !speechConfiguration || !previewText.trim()
      || !draft.elevenlabs_voice_id.trim()) return;
    setPreviewError(null);
    const session = new TextToSpeechSession(
      speechConfiguration,
      speechVoice(draft),
      previewText.trim(),
      () => {
        if (preview.current === session) preview.current = null;
        setPreviewing(false);
      },
      undefined,
      undefined,
      undefined,
      nativeSpeechFromClient(client),
      undefined,
      { onError: (failure) => setPreviewError(failure.message) },
    );
    preview.current = session;
    setPreviewing(true);
    try {
      await session.play();
    } catch (failure: unknown) {
      if (preview.current !== session) return;
      session.stop();
      preview.current = null;
      setPreviewing(false);
      setPreviewError(failure instanceof TextToSpeechError
        ? failure.message : 'Voice preview could not be played.');
    }
  }

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!id || !detail || !draft || saving || deleting) return;
    setSaving(true);
    setError(null);
    try {
      const updated = await client.updateVoice(id, draft);
      setDetail(updated);
      setDraft(voiceUpdate(updated));
      reload.remember(updated);
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Voice settings could not be saved.'));
    } finally {
      setSaving(false);
    }
  }

  async function remove() {
    setConfirming(false);
    if (!id || saving || deleting) return;
    setDeleting(true);
    setError(null);
    try {
      await client.deleteVoice(id);
      navigate({ type: 'show-settings-voices' });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The voice could not be deleted.'));
      setDeleting(false);
    }
  }

  const dirty = Boolean(detail && draft
    && JSON.stringify(voiceUpdate(detail)) !== JSON.stringify(draft));
  reload.markDirty(dirty);
  function resetVoice() {
    if (reload.stale) {
      reload.accept(applyVoice);
      return;
    }
    if (detail) setDraft(voiceUpdate(detail));
  }
  const disabled = saving || deleting || !detail?.writable;
  const supportsSpeed = draft?.provider !== 'elevenlabs' || voiceOutput?.elevenlabs?.supports_speed !== false;
  return (
    <section className="cha-screen cha-navigation" aria-label="Voice settings">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-voices' })} type="button"><ChevronLeftIcon /><span>Voices</span></button>
      {(detail || state.inspectedVoice.name) && <div className="cha-detail-actions">
        {detail ? <DetailRefreshProvider value={reload.context}><EditableTitle
          available={detail.writable} disabled={saving || deleting}
          id={detail.id} name={detail.display_name} subject="Voice"
          onSave={async (display_name) => {
            setSaving(true);
            try {
              const saved = await client.updateVoice(detail.id, { ...voiceUpdate(detail), display_name });
              setDetail(saved);
              setDraft((current) => current && ({ ...current, display_name: saved.display_name }));
              reload.remember(saved);
              dispatch({ type: 'voice-updated', voiceId: saved.id,
                voiceName: saved.display_name, writable: saved.writable });
            } finally {
              setSaving(false);
            }
          }}
        /></DetailRefreshProvider> : <h1>{state.inspectedVoice.name}</h1>}
      </div>}
      {!id && <p className="cha-state-message">No voice is selected.</p>}
      {id && !detail && !error && <p className="cha-state-message" role="status">Loading voice…</p>}
      {error && (!detail || reload.refreshFailed) && <LoadFailure message={error} retry={reload.retry} />}
      {detail && draft && (
        <form className="cha-settings-form cha-voice-settings-form" onSubmit={(event) => void save(event)}>
          <label htmlFor="cha-voice-description">Description<textarea className="cha-form-control cha-voice-description" disabled={disabled} id="cha-voice-description" onChange={(event) => change('description', event.target.value)} value={draft.description} /></label>
          <label>Provider<select className="cha-form-control" disabled={disabled} value={draft.provider ?? 'fishaudio'} onChange={(event) => change('provider', event.target.value as VoiceUpdate['provider'])}><option value="fishaudio">FishAudio</option><option value="elevenlabs">ElevenLabs</option></select></label>
          <label htmlFor="cha-voice-reference-id">Voice ID<input className="cha-form-control" disabled={disabled} id="cha-voice-reference-id" onChange={(event) => change('elevenlabs_voice_id', event.target.value)} value={draft.elevenlabs_voice_id} /></label>
          {draft.provider === 'elevenlabs' && voiceOutputError
            && <LoadFailure message={voiceOutputError} retry={retryVoiceOutput} />}
          {supportsSpeed && (
            <>
              <h2 className="cha-settings-section-title">Delivery</h2>
              <div className="cha-settings-form-grid">
                <label>Speed <span>0.7–1.2</span><input className="cha-form-control" disabled={disabled} max="1.2" min="0.7" onChange={(event) => change('speed', optionalNumber(event.target.value))} step="0.01" type="number" value={draft.speed ?? ''} /></label>
              </div>
            </>
          )}
          <h2 className="cha-settings-section-title">Preview</h2>
          <label htmlFor="cha-voice-preview-text">Text to speak<textarea className="cha-form-control cha-voice-preview-text" id="cha-voice-preview-text" onChange={(event) => setPreviewText(event.target.value)} value={previewText} /></label>
          {previewError && <p className="cha-error-message" role="alert">{previewError}</p>}
          {speechConfiguration && <div className="cha-settings-form-actions"><button className="cha-button cha-voice-preview-action" disabled={!previewing && (!previewText.trim() || !draft.elevenlabs_voice_id.trim())} onClick={() => void togglePreview()} type="button">{previewing ? <><StopIcon /> Stop preview</> : <><SpeakerIcon /> Play preview</>}</button></div>}
          <UsedBy empty="Nothing uses this voice." items={detail.used_by} />
          {error && !reload.refreshFailed && <p className="cha-error-message" role="alert">{error}</p>}
          {reload.stale && <StaleNotice onReload={() => reload.accept(applyVoice)} />}
          <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={(!dirty && !reload.stale) || saving || deleting} onClick={resetVoice} type="button">Reset</button><button className="cha-button cha-button-primary" disabled={!dirty || disabled || !draft.elevenlabs_voice_id.trim() || reload.blocked} type="submit">{saving ? 'Saving…' : 'Save voice'}</button></div>
          <div className="cha-settings-form-actions"><button className="cha-button cha-button-danger" disabled={saving || deleting || !detail.writable} onClick={() => setConfirming(true)} type="button">{deleting ? 'Deleting…' : 'Delete voice'}</button></div>
        </form>
      )}
      {confirming && (
        <ConfirmDialog confirmLabel="Delete voice" message={`Delete “${detail?.display_name ?? 'this voice'}”? This cannot be undone.`} onCancel={() => setConfirming(false)} onConfirm={() => void remove()} title="Delete voice?" />
      )}
    </section>
  );
}
