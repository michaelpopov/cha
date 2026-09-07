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

function UsedBy({ empty, items }: { empty: string; items: string[] }) {
  return (
    <section className="cha-settings-usage">
      <h2>Used by</h2>
      {items.length
        ? <ul>{items.map((item) => <li key={item}>{item}</li>)}</ul>
        : <p>{empty}</p>}
    </section>
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
              onClick={() => dispatch({
                type: 'inspect-provider',
                providerId: provider.id,
                providerName: provider.display_name,
              })}
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
      dispatch({
        type: 'inspect-provider',
        providerId: created.id,
        providerName: created.display_name,
      });
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
  const { id: _id, used_by: _usedBy, writable: _writable, ...update } = detail;
  return update;
}

const openAiOAuthCredential = 'openai_oauth';

function providerDraft(detail: ProviderDetail, keys: ApiKeyDetail[]): ProviderUpdate {
  const update = providerUpdate(detail);
  const usesOpenAiOAuth = update.auth === 'openai_subscription';
  return {
    ...update,
    api_key: !usesOpenAiOAuth && keys.some(({ id }) => id === update.api_key)
      ? update.api_key : null,
    api_key_env: null,
    auth: usesOpenAiOAuth ? 'openai_subscription' : 'none',
  };
}

function providerBaseUrl(provider: ProviderUpdate): string {
  const defaultPort = provider.https ? 443 : 80;
  const port = provider.port === defaultPort ? '' : `:${provider.port}`;
  return `${provider.https ? 'https' : 'http'}://${provider.host}${port}${provider.base_path}`;
}

function parseProviderBaseUrl(value: string): {
  host: string;
  port: number;
  basePath: string;
  https: boolean;
} | null {
  try {
    const url = new URL(value);
    const https = url.protocol === 'https:';
    if ((!https && url.protocol !== 'http:') || url.username || url.password
        || url.search || url.hash) {
      return null;
    }
    const basePath = url.pathname === '/'
      ? '' : url.pathname.replace(/\/+$/, '');
    const port = url.port ? Number(url.port) : (https ? 443 : 80);
    return url.hostname ? { host: url.hostname, port, basePath, https } : null;
  } catch {
    return null;
  }
}

interface ProviderScreenProps extends SettingsScreenProps {
  reloadVersion?: number;
}

export function ProviderScreen({
  client,
  dispatch,
  reloadVersion = 0,
  sessionReport,
  state,
}: ProviderScreenProps) {
  const id = state.inspectedProviderId;
  const [detail, setDetail] = useState<ProviderDetail | null>(null);
  const [draft, setDraft] = useState<ProviderUpdate | null>(null);
  const [baseUrl, setBaseUrl] = useState('');
  const [keys, setKeys] = useState<ApiKeyDetail[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [testing, setTesting] = useState(false);
  const [testSucceeded, setTestSucceeded] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const [revision, setRevision] = useState(0);

  useEffect(() => {
    let current = true;
    setDetail(null);
    setDraft(null);
    setError(null);
    setTestSucceeded(false);
    if (!id) return () => { current = false; };
    void Promise.all([client.getProvider(id), client.listApiKeys()]).then(
      ([provider, loadedKeys]) => {
        if (!current) return;
        setDetail(provider);
        setDraft(providerDraft(provider, loadedKeys));
        setBaseUrl(providerBaseUrl(provider));
        setKeys(loadedKeys);
        dispatch({
          type: 'provider-detail-loaded',
          providerId: provider.id,
          providerName: provider.display_name,
          writable: provider.writable,
        });
      },
      (failure: unknown) => {
        if (current) setError(publicErrorMessage(failure, 'Provider settings could not be loaded.'));
      },
    );
    return () => { current = false; };
  }, [client, dispatch, id, reloadVersion, revision]);

  function change<Key extends keyof ProviderUpdate>(key: Key, value: ProviderUpdate[Key]) {
    setDraft((current) => current ? { ...current, [key]: value } : current);
    setError(null);
    setTestSucceeded(false);
  }

  function changeCredential(value: string) {
    setDraft((current) => current ? {
      ...current,
      api_key: value === openAiOAuthCredential ? null : value || null,
      api_key_env: null,
      auth: value === openAiOAuthCredential ? 'openai_subscription' : 'none',
    } : current);
    setError(null);
    setTestSucceeded(false);
  }

  function candidateUpdate(): ProviderUpdate | null {
    if (!draft) return null;
    const connection = parseProviderBaseUrl(baseUrl);
    if (!connection) {
      setError('Base URL must be an HTTP or HTTPS address without credentials, query, or fragment.');
      return null;
    }
    const usesOpenAiOAuth = draft.auth === 'openai_subscription';
    if (usesOpenAiOAuth
        && (!connection.https
          || connection.port !== 443
          || connection.host !== 'chatgpt.com'
          || connection.basePath !== '/backend-api/codex')) {
      setError('OpenAI OAuth requires Base URL https://chatgpt.com/backend-api/codex.');
      return null;
    }
    if (usesOpenAiOAuth && draft.api !== 'responses') {
      setError('OpenAI OAuth requires the Responses API format.');
      return null;
    }
    return {
      ...draft,
      host: connection.host,
      port: connection.port,
      base_path: connection.basePath,
      mode: 'net',
      https: connection.https,
      stream: true,
      api_key: usesOpenAiOAuth ? null : draft.api_key,
      api_key_env: null,
      auth: usesOpenAiOAuth ? 'openai_subscription' : 'none',
      reasoning_effort: '',
      web_search: 'off',
      temperature: usesOpenAiOAuth ? null : draft.temperature,
      max_tokens: usesOpenAiOAuth ? null : draft.max_tokens,
      cache_retention: usesOpenAiOAuth ? 'off' : draft.cache_retention,
    };
  }

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!id || saving || deleting) return;
    const candidate = candidateUpdate();
    if (!candidate) return;
    setSaving(true);
    setError(null);
    try {
      const updated = await client.updateProvider(id, candidate);
      setDetail(updated);
      setDraft(providerDraft(updated, keys));
      setBaseUrl(providerBaseUrl(updated));
      setTestSucceeded(false);
      dispatch({
        type: 'provider-updated',
        providerId: updated.id,
        providerName: updated.display_name,
        writable: updated.writable,
      });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Provider settings could not be saved.'));
    } finally {
      setSaving(false);
    }
  }

  async function runTest() {
    if (!id || saving || testing || deleting) return;
    const candidate = candidateUpdate();
    if (!candidate) return;
    setTesting(true);
    setTestSucceeded(false);
    setError(null);
    try {
      await client.testProvider(id, candidate);
      setTestSucceeded(true);
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Provider test failed.'));
    } finally {
      setTesting(false);
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

  const dirty = detail !== null && draft !== null && (
    draft.model !== detail.model
    || draft.api !== detail.api
    || draft.auth !== detail.auth
    || draft.api_key !== (keys.some(({ id: keyId }) => keyId === detail.api_key)
      ? detail.api_key : null)
    || baseUrl !== providerBaseUrl(detail)
  );

  function reset() {
    if (!detail) return;
    setDraft(providerDraft(detail, keys));
    setBaseUrl(providerBaseUrl(detail));
    setError(null);
    setTestSucceeded(false);
  }

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
          <fieldset aria-label="Provider details" disabled={saving || testing || deleting || !detail.writable}>
            <label>Model<input className="cha-form-control" onChange={(event) => change('model', event.target.value)} value={draft.model} /></label>
            <label>Base URL<input className="cha-form-control" onChange={(event) => { setBaseUrl(event.target.value); setError(null); setTestSucceeded(false); }} placeholder="https://api.openai.com" type="url" value={baseUrl} /></label>
            <div className="cha-settings-form-grid">
              <label>API format<select className="cha-form-control" onChange={(event) => change('api', event.target.value as ProviderUpdate['api'])} value={draft.api}><option value="responses">Responses</option><option value="chat_completions">Chat completions</option></select></label>
              <label>Credentials<select className="cha-form-control" onChange={(event) => changeCredential(event.target.value)} value={draft.auth === 'openai_subscription'
                ? openAiOAuthCredential : draft.api_key ?? ''}><option value="">No credentials</option><option value={openAiOAuthCredential}>OpenAI OAuth</option>{keys.map((key) => <option key={key.id} value={key.id}>{key.display_name}</option>)}</select></label>
            </div>
          </fieldset>
          {!detail.writable && <p>This provider is read-only.</p>}
          <UsedBy empty="No characters use this provider." items={detail.used_by} />
          {error && <p className="cha-error-message" role="alert">{error}</p>}
          {testSucceeded && <p className="cha-settings-saved" role="status"><span aria-hidden="true" className="cha-settings-status-marker" /> Provider responded successfully.</p>}
          <div className="cha-settings-form-actions">
            <button className="cha-button" disabled={saving || testing || deleting} onClick={() => void runTest()} type="button">{testing ? 'Testing…' : 'Test'}</button>
            <button className="cha-button cha-button-ghost" disabled={!dirty || saving || testing || deleting} onClick={reset} type="button">Cancel</button>
            <button className="cha-button cha-button-primary cha-provider-save-action" disabled={!dirty || saving || testing || deleting || !detail.writable} type="submit">{saving ? 'Saving…' : 'Save changes'}</button>
          </div>
          <div className="cha-settings-form-actions">
            <button className="cha-button cha-button-danger cha-provider-delete-action" disabled={saving || testing || deleting || !detail.writable} onClick={() => setConfirming(true)} type="button">{deleting ? 'Deleting…' : 'Delete provider'}</button>
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
        <SettingsRow description={`${style.font} · ${style.weight} · ${style.size}`} icon={<CharacterIcon />} key={style.id} label={style.display_name} onClick={() => dispatch({ type: 'inspect-style', styleId: style.id, styleName: style.display_name })} />
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
      dispatch({ type: 'inspect-style', styleId: created.id, styleName: created.display_name });
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
  const { id: _id, used_by: _usedBy, writable: _writable, ...update } = detail;
  return update;
}

interface StyleScreenProps extends SettingsScreenProps {
  reloadVersion?: number;
}

export function StyleScreen({
  client,
  dispatch,
  reloadVersion = 0,
  sessionReport,
  state,
}: StyleScreenProps) {
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
        dispatch({
          type: 'style-detail-loaded',
          styleId: loaded.id,
          styleName: loaded.display_name,
          writable: loaded.writable,
        });
      },
      (failure: unknown) => { if (current) setError(publicErrorMessage(failure, 'Style settings could not be loaded.')); },
    );
    return () => { current = false; };
  }, [client, dispatch, id, reloadVersion, revision]);

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
          <fieldset aria-label="Style settings" disabled={saving || deleting || !detail.writable}>
            <div className="cha-settings-form-grid">
              <label>Font<select className="cha-form-control" onChange={(event) => change('font', event.target.value as StyleUpdate['font'])} value={draft.font}><option value="sans">Sans</option><option value="serif">Serif</option><option value="mono">Mono</option></select></label>
              <label>Slant<select className="cha-form-control" onChange={(event) => change('style', event.target.value as StyleUpdate['style'])} value={draft.style}><option value="normal">Normal</option><option value="italic">Italic</option></select></label>
              <label>Weight<select className="cha-form-control" onChange={(event) => change('weight', event.target.value as StyleUpdate['weight'])} value={draft.weight}><option value="light">Light</option><option value="normal">Normal</option><option value="medium">Medium</option><option value="semibold">Semibold</option><option value="bold">Bold</option></select></label>
              <label>Size<select className="cha-form-control" onChange={(event) => change('size', event.target.value as StyleUpdate['size'])} value={draft.size}><option value="small">Small</option><option value="normal">Normal</option><option value="large">Large</option></select></label>
              <label>Text color<select className="cha-form-control" onChange={(event) => change('text_color', event.target.value as StyleUpdate['text_color'])} value={draft.text_color}><option value="normal">Normal</option><option value="muted">Muted</option><option value="accent">Accent</option></select></label>
            </div>
          </fieldset>
          <p className={`cha-style-sample cha-message-text${appearance ? voiceClasses(appearance) : ''}`}>The chief task in life is this…</p>
          <UsedBy empty="No characters use this style." items={detail.used_by} />
          {error && <p className="cha-error-message" role="alert">{error}</p>}
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
      {keys && <div className="cha-list"><SettingsRow description="Save a secret on this device" icon={<PlusIcon />} label="New API key" onClick={() => dispatch({ type: 'show-settings-new-api-key' })} />{keys.length === 0 && <p className="cha-empty-list">No API keys saved</p>}{keys.map((key) => <SettingsRow description={key.used_by.length ? `Used by ${key.used_by.join(', ')}` : 'Saved locally · Not in use'} icon={<KeyIcon />} key={key.id} label={key.display_name} onClick={() => dispatch({ type: 'inspect-api-key', apiKeyId: key.id, apiKeyName: key.display_name })} />)}</div>}
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
      dispatch({ type: 'inspect-api-key', apiKeyId: created.id, apiKeyName: created.display_name });
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
  const [replacement, setReplacement] = useState('');
  const [busy, setBusy] = useState<'value' | 'delete' | null>(null);
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
        dispatch({
          type: 'api-key-detail-loaded',
          apiKeyId: loaded.id,
          apiKeyName: loaded.display_name,
        });
      },
      (failure: unknown) => { if (current) setError(publicErrorMessage(failure, 'The API key could not be loaded.')); },
    );
    return () => { current = false; };
  }, [client, dispatch, id, revision]);

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
      {key && <form className="cha-settings-form" onSubmit={(event) => void replace(event)}>
        <fieldset disabled={busy !== null}><label>New API key<input autoComplete="off" className="cha-form-control" onChange={(event) => setReplacement(event.target.value)} placeholder="Paste replacement key" type="password" value={replacement} /></label><div className="cha-settings-form-actions"><button className="cha-button cha-button-primary" disabled={!replacement || busy !== null} type="submit">{busy === 'value' ? 'Saving…' : 'Save'}</button></div></fieldset>
        <UsedBy empty="No providers reference this key." items={key.used_by} />
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions">
          <button className="cha-button cha-button-danger" disabled={busy !== null} onClick={() => setConfirming(true)} type="button">{busy === 'delete' ? 'Removing…' : 'Remove API key'}</button>
        </div>
      </form>}
      {confirming && (
        <ConfirmDialog
          confirmLabel="Remove API key"
          message={`Remove “${state.inspectedApiKeyName ?? 'this key'}” from this device? Providers using it will stop authenticating.`}
          onCancel={() => setConfirming(false)}
          onConfirm={() => void remove()}
          title="Remove API key?"
        />
      )}
    </section>
  );
}
