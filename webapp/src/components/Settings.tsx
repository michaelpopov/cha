import {
  useEffect,
  useState,
  type Dispatch,
  type FormEvent,
  type ReactNode,
} from 'react';

import {
  publicErrorMessage,
  type ApiKeyDetail,
  type ChaClient,
  type ProviderDetail,
  type ProviderSummary,
  type ProviderUpdate,
  type StyleDetail,
  type StyleUpdate,
} from '../api/client';
import type { AppAction, AppState } from '../state/view';
import { voiceClasses } from './characterAppearance';
import { ConfirmDialog } from './ConfirmDialog';
import {
  CharacterIcon,
  ChevronLeftIcon,
  ChevronRightIcon,
  KeyIcon,
  PlusIcon,
  SettingsIcon,
} from './Icons';

interface SettingsScreenProps {
  client: ChaClient;
  dispatch: Dispatch<AppAction>;
  sessionReport: ReactNode;
  state: AppState;
}

function BackToSettings({ dispatch }: { dispatch: Dispatch<AppAction> }) {
  return (
    <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings' })} type="button">
      <ChevronLeftIcon />
      <span>Settings</span>
    </button>
  );
}

function SettingsRow({
  description,
  icon,
  label,
  onClick,
}: {
  description: string;
  icon: ReactNode;
  label: string;
  onClick(): void;
}) {
  return (
    <button className="cha-list-action cha-settings-row" onClick={onClick} type="button">
      <span className="cha-list-icon">{icon}</span>
      <span className="cha-list-copy">
        <span className="cha-primary-line">{label}</span>
        <span className="cha-secondary-line">{description}</span>
      </span>
      <ChevronRightIcon className="cha-chevron" />
    </button>
  );
}

export function SettingsNavigation({ dispatch }: { dispatch: Dispatch<AppAction> }) {
  return (
    <section className="cha-settings-card" aria-labelledby="cha-configuration-settings-title">
      <header className="cha-settings-card-header">
        <h2 id="cha-configuration-settings-title">Configuration</h2>
        <p>Configure inference and how characters appear.</p>
      </header>
      <div className="cha-settings-links">
        <SettingsRow
          description="Endpoints, models, defaults, and authentication"
          icon={<SettingsIcon />}
          label="Providers"
          onClick={() => dispatch({ type: 'show-settings-providers' })}
        />
        <SettingsRow
          description="Typography and color presets for characters"
          icon={<CharacterIcon />}
          label="Styles"
          onClick={() => dispatch({ type: 'show-settings-styles' })}
        />
        <SettingsRow
          description="Secrets saved locally on this device"
          icon={<KeyIcon />}
          label="API Keys"
          onClick={() => dispatch({ type: 'show-settings-api-keys' })}
        />
      </div>
    </section>
  );
}

function LoadFailure({ message, retry }: { message: string; retry(): void }) {
  return (
    <div className="cha-state-message cha-error-message" role="alert">
      <p>{message}</p>
      <button className="cha-button cha-button-ghost" onClick={retry} type="button">Try again</button>
    </div>
  );
}

export function ProvidersScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const [providers, setProviders] = useState<ProviderSummary[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [revision, setRevision] = useState(0);

  useEffect(() => {
    let current = true;
    setProviders(null);
    setError(null);
    void client.listProviders().then(
      (loaded) => { if (current) setProviders(loaded); },
      (failure: unknown) => {
        if (current) setError(publicErrorMessage(failure, 'Providers could not be loaded.'));
      },
    );
    return () => { current = false; };
  }, [client, revision]);

  return (
    <section className="cha-screen cha-navigation" aria-label="Providers settings">
      <BackToSettings dispatch={dispatch} />
      {sessionReport}
      {providers === null && !error && <p className="cha-state-message" role="status">Loading providers…</p>}
      {error && <LoadFailure message={error} retry={() => setRevision((value) => value + 1)} />}
      {providers && (
        <div className="cha-list">
          <SettingsRow
            description="Start with a basic OpenAI-compatible provider"
            icon={<PlusIcon />}
            label="New provider"
            onClick={() => dispatch({ type: 'show-settings-new-provider' })}
          />
          {providers.length === 0 && <p className="cha-empty-list">No providers configured</p>}
          {providers.map((provider) => (
            <SettingsRow
              description={`${provider.model} · ${provider.host}`}
              icon={<SettingsIcon />}
              key={provider.id}
              label={provider.display_name}
              onClick={() => dispatch({ type: 'inspect-provider', providerId: provider.id })}
            />
          ))}
        </div>
      )}
    </section>
  );
}

export function NewProviderScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const [name, setName] = useState('');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!name.trim() || saving) return;
    setSaving(true);
    setError(null);
    try {
      const created = await client.createProvider({ display_name: name.trim() });
      dispatch({ type: 'inspect-provider', providerId: created.id });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The provider could not be created.'));
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="New provider settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-providers' })} type="button"><ChevronLeftIcon /><span>Providers</span></button>
      {sessionReport}
      <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
        <fieldset disabled={saving}><legend>Provider details</legend><label>Name<input autoFocus className="cha-form-control" onChange={(event) => setName(event.target.value)} placeholder="e.g. OpenRouter" value={name} /></label></fieldset>
        <p className="cha-settings-note">After creation, you can configure the endpoint, model, and API key.</p>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving} onClick={() => dispatch({ type: 'show-settings-providers' })} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!name.trim() || saving} type="submit">{saving ? 'Creating…' : 'Create provider'}</button></div>
      </form>
    </section>
  );
}

function providerUpdate(detail: ProviderDetail): ProviderUpdate {
  const { id: _id, writable: _writable, ...update } = detail;
  return update;
}

function optionalNumber(value: string): number | null {
  if (value === '') return null;
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : null;
}

export function ProviderScreen({ client, dispatch, sessionReport, state }: SettingsScreenProps) {
  const id = state.inspectedProviderId;
  const [detail, setDetail] = useState<ProviderDetail | null>(null);
  const [draft, setDraft] = useState<ProviderUpdate | null>(null);
  const [keys, setKeys] = useState<ApiKeyDetail[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const [revision, setRevision] = useState(0);

  useEffect(() => {
    let current = true;
    setDetail(null);
    setDraft(null);
    setError(null);
    if (!id) return () => { current = false; };
    void Promise.all([client.getProvider(id), client.listApiKeys()]).then(
      ([provider, loadedKeys]) => {
        if (!current) return;
        setDetail(provider);
        setDraft(providerUpdate(provider));
        setKeys(loadedKeys);
      },
      (failure: unknown) => {
        if (current) setError(publicErrorMessage(failure, 'Provider settings could not be loaded.'));
      },
    );
    return () => { current = false; };
  }, [client, id, revision]);

  function change<Key extends keyof ProviderUpdate>(key: Key, value: ProviderUpdate[Key]) {
    setDraft((current) => current ? { ...current, [key]: value } : current);
  }

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!id || !draft || saving || deleting) return;
    setSaving(true);
    setError(null);
    try {
      const updated = await client.updateProvider(id, draft);
      setDetail(updated);
      setDraft(providerUpdate(updated));
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Provider settings could not be saved.'));
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
      await client.deleteProvider(id);
      dispatch({ type: 'show-settings-providers' });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The provider could not be deleted.'));
      setDeleting(false);
    }
  }

  const dirty = detail !== null && draft !== null
    && JSON.stringify(providerUpdate(detail)) !== JSON.stringify(draft);
  const missingKey = draft?.api_key && !keys.some(({ id: keyId }) => keyId === draft.api_key);

  return (
    <section className="cha-screen cha-navigation" aria-label="Provider settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-providers' })} type="button">
        <ChevronLeftIcon /><span>Providers</span>
      </button>
      {sessionReport}
      {!id && <p className="cha-state-message">No provider is selected.</p>}
      {id && !detail && !error && <p className="cha-state-message" role="status">Loading provider…</p>}
      {error && !detail && <LoadFailure message={error} retry={() => setRevision((value) => value + 1)} />}
      {detail && draft && (
        <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
          <fieldset disabled={saving || deleting || !detail.writable}>
            <legend>Identity</legend>
            <label>Name<input className="cha-form-control" onChange={(event) => change('display_name', event.target.value)} value={draft.display_name} /></label>
            <label>Model<input className="cha-form-control" onChange={(event) => change('model', event.target.value)} value={draft.model} /></label>
          </fieldset>
          <fieldset disabled={saving || deleting || !detail.writable}>
            <legend>Connection</legend>
            <label>Host<input className="cha-form-control" onChange={(event) => change('host', event.target.value)} value={draft.host} /></label>
            <div className="cha-settings-form-grid">
              <label>Port<input className="cha-form-control" min="1" max="65535" onChange={(event) => change('port', Number(event.target.value))} type="number" value={draft.port} /></label>
              <label>Protocol<select className="cha-form-control" onChange={(event) => change('https', event.target.value === 'https')} value={draft.https ? 'https' : 'http'}><option value="https">HTTPS</option><option value="http">HTTP</option></select></label>
            </div>
            <label>Base path<input className="cha-form-control" onChange={(event) => change('base_path', event.target.value)} placeholder="/api" value={draft.base_path} /></label>
            <div className="cha-settings-form-grid">
              <label>API<select className="cha-form-control" onChange={(event) => change('api', event.target.value as ProviderUpdate['api'])} value={draft.api}><option value="responses">Responses</option><option value="chat_completions">Chat completions</option></select></label>
              <label>Mode<select className="cha-form-control" onChange={(event) => change('mode', event.target.value as ProviderUpdate['mode'])} value={draft.mode}><option value="net">Network</option><option value="test">Test</option></select></label>
            </div>
          </fieldset>
          <fieldset disabled={saving || deleting || !detail.writable}>
            <legend>Authentication</legend>
            <label>Authentication<select className="cha-form-control" onChange={(event) => { const auth = event.target.value as ProviderUpdate['auth']; change('auth', auth); if (auth === 'openai_subscription') { change('api_key', null); change('api_key_env', null); } }} value={draft.auth}><option value="none">API key or none</option><option value="openai_subscription">ChatGPT subscription</option></select></label>
            <label>Saved API key<select className="cha-form-control" disabled={draft.auth === 'openai_subscription'} onChange={(event) => { change('api_key', event.target.value || null); if (event.target.value) change('api_key_env', null); }} value={draft.api_key ?? ''}><option value="">No saved key</option>{keys.map((key) => <option key={key.id} value={key.id}>{key.display_name}</option>)}{missingKey && <option value={draft.api_key!}>Missing key ({draft.api_key})</option>}</select></label>
            <label>Environment variable<input className="cha-form-control" disabled={draft.auth === 'openai_subscription'} onChange={(event) => { change('api_key_env', event.target.value || null); if (event.target.value) change('api_key', null); }} placeholder="Optional legacy setting" value={draft.api_key_env ?? ''} /></label>
          </fieldset>
          <fieldset disabled={saving || deleting || !detail.writable}>
            <legend>Defaults</legend>
            <div className="cha-settings-form-grid">
              <label>Temperature<input className="cha-form-control" max="2" min="0" onChange={(event) => change('temperature', optionalNumber(event.target.value))} step="0.1" type="number" value={draft.temperature ?? ''} /></label>
              <label>Maximum tokens<input className="cha-form-control" min="1" onChange={(event) => change('max_tokens', optionalNumber(event.target.value))} type="number" value={draft.max_tokens ?? ''} /></label>
              <label>Timeout (seconds)<input className="cha-form-control" min="1" onChange={(event) => change('timeout_s', Number(event.target.value))} type="number" value={draft.timeout_s} /></label>
              <label>Idle timeout (seconds)<input className="cha-form-control" min="1" onChange={(event) => change('idle_timeout_s', Number(event.target.value))} type="number" value={draft.idle_timeout_s} /></label>
            </div>
            <label>Reasoning effort<input className="cha-form-control" onChange={(event) => change('reasoning_effort', event.target.value)} placeholder="Provider default" value={draft.reasoning_effort} /></label>
            <div className="cha-settings-form-grid">
              <label>Reasoning format<select className="cha-form-control" onChange={(event) => change('reasoning_format', event.target.value as ProviderUpdate['reasoning_format'])} value={draft.reasoning_format}><option value="auto">Automatic</option><option value="none">None</option><option value="reasoning_content">reasoning_content</option><option value="reasoning">reasoning</option></select></label>
              <label>Web search<select className="cha-form-control" onChange={(event) => change('web_search', event.target.value as ProviderUpdate['web_search'])} value={draft.web_search}><option value="off">Off</option><option value="auto">Automatic</option><option value="required">Required</option></select></label>
              <label>Cache retention<select className="cha-form-control" onChange={(event) => change('cache_retention', event.target.value as ProviderUpdate['cache_retention'])} value={draft.cache_retention}><option value="off">Off</option><option value="short">Short</option><option value="long">Long</option></select></label>
              <label className="cha-settings-check"><input checked={draft.stream} onChange={(event) => change('stream', event.target.checked)} type="checkbox" /> Stream responses</label>
            </div>
          </fieldset>
          {!detail.writable && <p>This provider is read-only.</p>}
          {error && <p className="cha-error-message" role="alert">{error}</p>}
          <p className="cha-settings-note">Saving restarts sessions that use this provider. Providers assigned to characters cannot be deleted.</p>
          <div className="cha-settings-form-actions">
            <button className="cha-button cha-button-ghost" disabled={!dirty || saving || deleting} onClick={() => setDraft(providerUpdate(detail))} type="button">Reset</button>
            <button className="cha-button cha-button-primary" disabled={!dirty || saving || deleting || !detail.writable} type="submit">{saving ? 'Saving…' : 'Save provider'}</button>
          </div>
          <div className="cha-settings-form-actions">
            <button className="cha-button cha-button-danger" disabled={saving || deleting || !detail.writable} onClick={() => setConfirming(true)} type="button">{deleting ? 'Deleting…' : 'Delete provider'}</button>
          </div>
        </form>
      )}
      {confirming && (
        <ConfirmDialog
          confirmLabel="Delete provider"
          message={`Delete “${detail?.display_name ?? 'this provider'}”? This cannot be undone.`}
          onCancel={() => setConfirming(false)}
          onConfirm={() => void remove()}
          title="Delete provider?"
        />
      )}
    </section>
  );
}

export function StylesScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const [styles, setStyles] = useState<StyleDetail[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [revision, setRevision] = useState(0);
  useEffect(() => {
    let current = true;
    setStyles(null);
    setError(null);
    void client.listStyles().then(
      (loaded) => { if (current) setStyles(loaded); },
      (failure: unknown) => { if (current) setError(publicErrorMessage(failure, 'Styles could not be loaded.')); },
    );
    return () => { current = false; };
  }, [client, revision]);
  return (
    <section className="cha-screen cha-navigation" aria-label="Styles settings">
      <BackToSettings dispatch={dispatch} />
      {sessionReport}
      {styles === null && !error && <p className="cha-state-message" role="status">Loading styles…</p>}
      {error && <LoadFailure message={error} retry={() => setRevision((value) => value + 1)} />}
      {styles && <div className="cha-list"><SettingsRow description="Start with a neutral character style" icon={<PlusIcon />} label="New style" onClick={() => dispatch({ type: 'show-settings-new-style' })} />{styles.length === 0 && <p className="cha-empty-list">No styles configured</p>}{styles.map((style) => (
        <SettingsRow description={`${style.font} · ${style.weight} · ${style.size}`} icon={<CharacterIcon />} key={style.id} label={style.display_name} onClick={() => dispatch({ type: 'inspect-style', styleId: style.id })} />
      ))}</div>}
    </section>
  );
}

export function NewStyleScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const [name, setName] = useState('');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!name.trim() || saving) return;
    setSaving(true);
    setError(null);
    try {
      const created = await client.createStyle({ display_name: name.trim() });
      dispatch({ type: 'inspect-style', styleId: created.id });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The style could not be created.'));
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="New style settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-styles' })} type="button"><ChevronLeftIcon /><span>Styles</span></button>
      {sessionReport}
      <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
        <fieldset disabled={saving}><legend>Style details</legend><label>Name<input autoFocus className="cha-form-control" onChange={(event) => setName(event.target.value)} placeholder="e.g. Editorial" value={name} /></label></fieldset>
        <p className="cha-settings-note">After creation, you can choose typography, size, and color.</p>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving} onClick={() => dispatch({ type: 'show-settings-styles' })} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!name.trim() || saving} type="submit">{saving ? 'Creating…' : 'Create style'}</button></div>
      </form>
    </section>
  );
}

function styleUpdate(detail: StyleDetail): StyleUpdate {
  const { id: _id, writable: _writable, ...update } = detail;
  return update;
}

export function StyleScreen({ client, dispatch, sessionReport, state }: SettingsScreenProps) {
  const id = state.inspectedStyleId;
  const [detail, setDetail] = useState<StyleDetail | null>(null);
  const [draft, setDraft] = useState<StyleUpdate | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const [revision, setRevision] = useState(0);
  useEffect(() => {
    let current = true;
    setDetail(null);
    setDraft(null);
    setError(null);
    if (!id) return () => { current = false; };
    void client.listStyles().then(
      (styles) => {
        if (!current) return;
        const loaded = styles.find((style) => style.id === id);
        if (!loaded) return setError('That style was not found.');
        setDetail(loaded);
        setDraft(styleUpdate(loaded));
      },
      (failure: unknown) => { if (current) setError(publicErrorMessage(failure, 'Style settings could not be loaded.')); },
    );
    return () => { current = false; };
  }, [client, id, revision]);

  function change<Key extends keyof StyleUpdate>(key: Key, value: StyleUpdate[Key]) {
    setDraft((current) => current ? { ...current, [key]: value } : current);
  }
  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!id || !draft || saving || deleting) return;
    setSaving(true);
    setError(null);
    try {
      const updated = await client.updateStyle(id, draft);
      setDetail(updated);
      setDraft(styleUpdate(updated));
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Style settings could not be saved.'));
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
      await client.deleteStyle(id);
      dispatch({ type: 'show-settings-styles' });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The style could not be deleted.'));
      setDeleting(false);
    }
  }
  const dirty = detail && draft && JSON.stringify(styleUpdate(detail)) !== JSON.stringify(draft);
  const appearance = draft && {
    font: draft.font,
    style: draft.style,
    weight: draft.weight,
    size: draft.size,
    text_color: draft.text_color,
  };
  return (
    <section className="cha-screen cha-navigation" aria-label="Style settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-styles' })} type="button"><ChevronLeftIcon /><span>Styles</span></button>
      {sessionReport}
      {!id && <p className="cha-state-message">No style is selected.</p>}
      {id && !detail && !error && <p className="cha-state-message" role="status">Loading style…</p>}
      {error && !detail && <LoadFailure message={error} retry={() => setRevision((value) => value + 1)} />}
      {detail && draft && (
        <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
          <fieldset disabled={saving || deleting || !detail.writable}>
            <legend>Style</legend>
            <label>Name<input className="cha-form-control" onChange={(event) => change('display_name', event.target.value)} value={draft.display_name} /></label>
            <div className="cha-settings-form-grid">
              <label>Font<select className="cha-form-control" onChange={(event) => change('font', event.target.value as StyleUpdate['font'])} value={draft.font}><option value="sans">Sans</option><option value="serif">Serif</option><option value="mono">Mono</option></select></label>
              <label>Slant<select className="cha-form-control" onChange={(event) => change('style', event.target.value as StyleUpdate['style'])} value={draft.style}><option value="normal">Normal</option><option value="italic">Italic</option></select></label>
              <label>Weight<select className="cha-form-control" onChange={(event) => change('weight', event.target.value as StyleUpdate['weight'])} value={draft.weight}><option value="light">Light</option><option value="normal">Normal</option><option value="medium">Medium</option><option value="semibold">Semibold</option><option value="bold">Bold</option></select></label>
              <label>Size<select className="cha-form-control" onChange={(event) => change('size', event.target.value as StyleUpdate['size'])} value={draft.size}><option value="small">Small</option><option value="normal">Normal</option><option value="large">Large</option></select></label>
              <label>Text color<select className="cha-form-control" onChange={(event) => change('text_color', event.target.value as StyleUpdate['text_color'])} value={draft.text_color}><option value="normal">Normal</option><option value="muted">Muted</option><option value="accent">Accent</option></select></label>
            </div>
          </fieldset>
          <p className={`cha-style-sample cha-message-text${appearance ? voiceClasses(appearance) : ''}`}>The chief task in life is this…</p>
          {error && <p className="cha-error-message" role="alert">{error}</p>}
          <p className="cha-settings-note">Saving restarts sessions whose characters use this style. Styles assigned to characters cannot be deleted.</p>
          <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={!dirty || saving || deleting} onClick={() => setDraft(styleUpdate(detail))} type="button">Reset</button><button className="cha-button cha-button-primary" disabled={!dirty || saving || deleting || !detail.writable} type="submit">{saving ? 'Saving…' : 'Save style'}</button></div>
          <div className="cha-settings-form-actions">
            <button className="cha-button cha-button-danger" disabled={saving || deleting || !detail.writable} onClick={() => setConfirming(true)} type="button">{deleting ? 'Deleting…' : 'Delete style'}</button>
          </div>
        </form>
      )}
      {confirming && (
        <ConfirmDialog
          confirmLabel="Delete style"
          message={`Delete “${detail?.display_name ?? 'this style'}”? This cannot be undone.`}
          onCancel={() => setConfirming(false)}
          onConfirm={() => void remove()}
          title="Delete style?"
        />
      )}
    </section>
  );
}

export function ApiKeysScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const [keys, setKeys] = useState<ApiKeyDetail[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [revision, setRevision] = useState(0);
  useEffect(() => {
    let current = true;
    setKeys(null);
    setError(null);
    void client.listApiKeys().then(
      (loaded) => { if (current) setKeys(loaded); },
      (failure: unknown) => { if (current) setError(publicErrorMessage(failure, 'API keys could not be loaded.')); },
    );
    return () => { current = false; };
  }, [client, revision]);
  return (
    <section className="cha-screen cha-navigation" aria-label="API keys settings">
      <BackToSettings dispatch={dispatch} />
      {sessionReport}
      {keys === null && !error && <p className="cha-state-message" role="status">Loading API keys…</p>}
      {error && <LoadFailure message={error} retry={() => setRevision((value) => value + 1)} />}
      {keys && <div className="cha-list"><SettingsRow description="Save a secret on this device" icon={<PlusIcon />} label="New API key" onClick={() => dispatch({ type: 'show-settings-new-api-key' })} />{keys.length === 0 && <p className="cha-empty-list">No API keys saved</p>}{keys.map((key) => <SettingsRow description={key.used_by.length ? `Used by ${key.used_by.join(', ')}` : 'Saved locally · Not in use'} icon={<KeyIcon />} key={key.id} label={key.display_name} onClick={() => dispatch({ type: 'inspect-api-key', apiKeyId: key.id })} />)}</div>}
    </section>
  );
}

export function NewApiKeyScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const [name, setName] = useState('');
  const [value, setValue] = useState('');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!name.trim() || !value || saving) return;
    setSaving(true);
    setError(null);
    try {
      const created = await client.createApiKey({ display_name: name.trim(), value });
      setValue('');
      dispatch({ type: 'inspect-api-key', apiKeyId: created.id });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The API key could not be saved.'));
      setSaving(false);
    }
  }
  return (
    <section className="cha-screen cha-navigation" aria-label="New API key settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-api-keys' })} type="button"><ChevronLeftIcon /><span>API Keys</span></button>
      {sessionReport}
      <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
        <fieldset disabled={saving}><legend>Key details</legend><label>Name<input autoFocus autoComplete="off" className="cha-form-control" onChange={(event) => setName(event.target.value)} placeholder="e.g. OpenRouter" value={name} /></label><label>API key<input autoComplete="off" className="cha-form-control" onChange={(event) => setValue(event.target.value)} placeholder="Paste key" type="password" value={value} /></label></fieldset>
        <p className="cha-settings-note">The value is stored only in the local application config and is never returned to the browser.</p>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving} onClick={() => dispatch({ type: 'show-settings-api-keys' })} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!name.trim() || !value || saving} type="submit">{saving ? 'Saving…' : 'Save API key'}</button></div>
      </form>
    </section>
  );
}

export function ApiKeyScreen({ client, dispatch, sessionReport, state }: SettingsScreenProps) {
  const id = state.inspectedApiKeyId;
  const [key, setKey] = useState<ApiKeyDetail | null>(null);
  const [name, setName] = useState('');
  const [replacement, setReplacement] = useState('');
  const [busy, setBusy] = useState<'name' | 'value' | 'delete' | null>(null);
  const [confirming, setConfirming] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [revision, setRevision] = useState(0);
  useEffect(() => {
    let current = true;
    setKey(null);
    setError(null);
    if (!id) return () => { current = false; };
    void client.listApiKeys().then(
      (keys) => {
        if (!current) return;
        const loaded = keys.find((candidate) => candidate.id === id);
        if (!loaded) return setError('That API key was not found.');
        setKey(loaded);
        setName(loaded.display_name);
      },
      (failure: unknown) => { if (current) setError(publicErrorMessage(failure, 'The API key could not be loaded.')); },
    );
    return () => { current = false; };
  }, [client, id, revision]);

  async function rename(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!id || !key || !name.trim() || busy) return;
    setBusy('name'); setError(null);
    try { const updated = await client.renameApiKey(id, name.trim()); setKey(updated); setName(updated.display_name); }
    catch (failure: unknown) { setError(publicErrorMessage(failure, 'The API key name could not be saved.')); }
    finally { setBusy(null); }
  }
  async function replace(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!id || !replacement || busy) return;
    setBusy('value'); setError(null);
    try { const updated = await client.replaceApiKeyValue(id, replacement); setKey(updated); setReplacement(''); }
    catch (failure: unknown) { setError(publicErrorMessage(failure, 'The API key value could not be replaced.')); }
    finally { setBusy(null); }
  }
  async function remove() {
    setConfirming(false);
    if (!id || busy) return;
    setBusy('delete'); setError(null);
    try { await client.deleteApiKey(id); dispatch({ type: 'show-settings-api-keys' }); }
    catch (failure: unknown) { setError(publicErrorMessage(failure, 'The API key could not be removed.')); setBusy(null); }
  }
  return (
    <section className="cha-screen cha-navigation" aria-label="API key settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-api-keys' })} type="button"><ChevronLeftIcon /><span>API Keys</span></button>
      {sessionReport}
      {!id && <p className="cha-state-message">No API key is selected.</p>}
      {id && !key && !error && <p className="cha-state-message" role="status">Loading API key…</p>}
      {error && !key && <LoadFailure message={error} retry={() => setRevision((value) => value + 1)} />}
      {key && <div className="cha-settings-form">
        <form onSubmit={(event) => void rename(event)}><fieldset disabled={busy !== null}><legend>Key details</legend><label>Name<input className="cha-form-control" onChange={(event) => setName(event.target.value)} value={name} /></label><p className="cha-settings-saved"><span className="cha-settings-status-marker" /> Saved locally</p><div className="cha-settings-form-actions"><button className="cha-button cha-button-primary" disabled={!name.trim() || name.trim() === key.display_name || busy !== null} type="submit">Save name</button></div></fieldset></form>
        <form onSubmit={(event) => void replace(event)}><fieldset disabled={busy !== null}><legend>Replace value</legend><label>New API key<input autoComplete="off" className="cha-form-control" onChange={(event) => setReplacement(event.target.value)} placeholder="Paste replacement key" type="password" value={replacement} /></label><div className="cha-settings-form-actions"><button className="cha-button cha-button-primary" disabled={!replacement || busy !== null} type="submit">Replace value</button></div></fieldset></form>
        <section className="cha-settings-usage"><h2>Used by</h2>{key.used_by.length ? <ul>{key.used_by.map((provider) => <li key={provider}>{provider}</li>)}</ul> : <p>No providers reference this key.</p>}</section>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions">
          <button className="cha-button cha-button-danger" disabled={busy !== null} onClick={() => setConfirming(true)} type="button">{busy === 'delete' ? 'Removing…' : 'Remove API key'}</button>
        </div>
      </div>}
      {confirming && (
        <ConfirmDialog
          confirmLabel="Remove API key"
          message={`Remove “${key?.display_name ?? 'this key'}” from this device? Providers using it will stop authenticating.`}
          onCancel={() => setConfirming(false)}
          onConfirm={() => void remove()}
          title="Remove API key?"
        />
      )}
    </section>
  );
}
