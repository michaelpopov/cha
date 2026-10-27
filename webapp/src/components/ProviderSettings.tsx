import { useEffect, useState, type FormEvent } from 'react';
import {
  publicErrorMessage,
  type ApiKeyDetail,
  type ChaClient,
  type ProviderDetail,
  type ProviderSummary,
  type ProviderUpdate,
} from '../api/client';
import { DetailRefreshProvider, StaleNotice, useFormReload } from '../detailRefresh';
import { useLoad } from '../useLoad';
import { ConfirmDialog } from './ConfirmDialog';
import { EditableTitle } from './DetailActions';
import { ChevronLeftIcon, PlusIcon, SettingsIcon } from './Icons';
import { TransliteratingInput } from './TransliterationMode';
import {
  BackToSettings,
  LoadFailure,
  SettingsRow,
  UsedBy,
  type SettingsScreenProps,
} from './SettingsShared';

const loadProviders = (client: ChaClient) => client.listProviders();

export function ProvidersScreen({ client, navigate }: SettingsScreenProps) {
  const { data: providers, error, retry } = useLoad(
    client, loadProviders, 'Providers could not be loaded.',
  );

  return (
    <section className="cha-screen cha-navigation" aria-label="Providers settings">
      <BackToSettings navigate={navigate} />
      {providers === null && !error && <p className="cha-state-message" role="status">Loading providers…</p>}
      {error && <LoadFailure message={error} retry={retry} />}
      {providers && (
        <div className="cha-list">
          <SettingsRow
            description="Start from defaults or copy existing settings"
            icon={<PlusIcon />}
            label="New provider"
            onClick={() => navigate({ type: 'show-settings-new-provider' })}
          />
          {providers.length === 0 && <p className="cha-empty-list">No providers configured</p>}
          {providers.map((provider) => (
            <SettingsRow
              description={`${provider.model} · ${provider.host}`}
              icon={<SettingsIcon />}
              key={provider.id}
              label={provider.display_name}
              onClick={() => navigate({
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

export function NewProviderScreen({ client, navigate }: SettingsScreenProps) {
  const [name, setName] = useState('');
  const [providers, setProviders] = useState<ProviderSummary[] | null>(null);
  const [copyFrom, setCopyFrom] = useState('');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let current = true;
    void client.listProviders().then(
      (loaded) => { if (current) setProviders(loaded); },
      (failure: unknown) => {
        if (current) setError(publicErrorMessage(failure, 'Providers could not be loaded.'));
      },
    );
    return () => { current = false; };
  }, [client]);

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!name.trim() || providers === null || saving) return;
    setSaving(true);
    setError(null);
    try {
      const created = await client.createProvider({
        display_name: name.trim(),
        copy_from: copyFrom || null,
      });
      navigate({
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
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-providers' })} type="button"><ChevronLeftIcon /><span>Providers</span></button>
      {providers === null && !error && <p className="cha-state-message" role="status">Loading providers…</p>}
      {providers !== null && (
        <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
          <fieldset disabled={saving}>
            <TransliteratingInput autoFocus className="cha-form-control" id="cha-new-provider-name" label="Name" onValueChange={setName} placeholder="e.g. OpenRouter" value={name} />
            <label>Initial settings<select className="cha-form-control" onChange={(event) => setCopyFrom(event.target.value)} value={copyFrom}><option value="">Default provider settings</option>{providers.map((provider) => <option key={provider.id} value={provider.id}>Copy {provider.display_name}</option>)}</select></label>
          </fieldset>
          {copyFrom && <p className="cha-settings-note">The selected provider settings, including its saved API-key selection, are copied.</p>}
          {error && <p className="cha-error-message" role="alert">{error}</p>}
          <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving} onClick={() => navigate({ type: 'show-settings-providers' })} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!name.trim() || saving} type="submit">{saving ? 'Creating…' : 'Create provider'}</button></div>
        </form>
      )}
      {error && providers === null && <p className="cha-error-message" role="alert">{error}</p>}
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
    auth: usesOpenAiOAuth ? 'openai_subscription' : 'none',
  };
}

function providerBaseUrl(provider: ProviderUpdate): string {
  const defaultPort = provider.https ? 443 : 80;
  const port = provider.port === defaultPort ? '' : `:${provider.port}`;
  return `${provider.https ? 'https' : 'http'}://${provider.host}${port}${provider.base_path}`;
}

function openRouterTargetsText(targets: string[] | undefined): string {
  return targets?.join('\n') ?? '';
}

function isOpenRouterHost(host: string): boolean {
  return host.replace(/\.$/, '').toLowerCase() === 'openrouter.ai';
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

function isOpenRouterBaseUrl(value: string): boolean {
  const connection = parseProviderBaseUrl(value);
  return connection !== null && isOpenRouterHost(connection.host);
}

export function ProviderScreen({
  client,
  dispatch,
  navigate,
  state,
}: SettingsScreenProps) {
  const id = state.inspectedProvider.id;
  const [detail, setDetail] = useState<ProviderDetail | null>(null);
  const [draft, setDraft] = useState<ProviderUpdate | null>(null);
  const [baseUrl, setBaseUrl] = useState('');
  const [openRouterTargets, setOpenRouterTargets] = useState('');
  const [keys, setKeys] = useState<ApiKeyDetail[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [testing, setTesting] = useState(false);
  const [testSucceeded, setTestSucceeded] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const reload = useFormReload(id);

  function applyProvider(loaded: { provider: ProviderDetail; keys: ApiKeyDetail[] }) {
    setDetail(loaded.provider);
    setDraft(providerDraft(loaded.provider, loaded.keys));
    setBaseUrl(providerBaseUrl(loaded.provider));
    setOpenRouterTargets(openRouterTargetsText(loaded.provider.openrouter_targets));
    setKeys(loaded.keys);
    dispatch({
      type: 'provider-detail-loaded',
      providerId: loaded.provider.id,
      providerName: loaded.provider.display_name,
      writable: loaded.provider.writable,
    });
  }

  useEffect(() => {
    let current = true;
    const background = reload.start();
    if (!background) {
      setDetail(null);
      setDraft(null);
      setOpenRouterTargets('');
      setError(null);
      setTestSucceeded(false);
    }
    if (!id) return () => { current = false; };
    void Promise.all([client.getProvider(id), client.listApiKeys()]).then(
      ([provider, loadedKeys]) => {
        if (!current) return;
        setError(null);
        reload.loaded({ provider, keys: loadedKeys }, applyProvider);
      },
      (failure: unknown) => {
        if (!current) return;
        setError(publicErrorMessage(failure, 'Provider settings could not be loaded.'));
        reload.fail(background);
      },
    );
    return () => { current = false; };
  }, [client, dispatch, id, reload.epoch, reload.attempt]);

  function change<Key extends keyof ProviderUpdate>(key: Key, value: ProviderUpdate[Key]) {
    setDraft((current) => {
      if (!current) return current;
      const next = { ...current, [key]: value };
      if (key === 'api' && (value === 'google' || value === 'anthropic') && current.auth === 'openai_subscription') {
        next.auth = 'none';
        next.api_key = null;
      }
      return next;
    });
    setError(null);
    setTestSucceeded(false);
  }

  function changeCredential(value: string) {
    setDraft((current) => current ? {
      ...current,
      api_key: value === openAiOAuthCredential ? null : value || null,
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
    const targets = draft.api !== 'google' && draft.api !== 'anthropic' && isOpenRouterHost(connection.host)
      ? openRouterTargets.split(/\r?\n/).map((target) => target.trim()).filter(Boolean)
      : [];
    if (targets.some((target) => /\s/.test(target))
        || new Set(targets.map((target) => target.toLowerCase())).size !== targets.length) {
      setError('Inference targets must be unique OpenRouter provider slugs, one per line.');
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
      auth: usesOpenAiOAuth ? 'openai_subscription' : 'none',
      web_search: 'off',
      reasoning_effort: (draft.api === 'google' || draft.api === 'anthropic') ? 'none' : draft.reasoning_effort,
      temperature: usesOpenAiOAuth || draft.api === 'anthropic' ? null : draft.temperature,
      reasoning_format: draft.api === 'anthropic' ? 'auto' : draft.reasoning_format,
      max_tokens: usesOpenAiOAuth ? null : draft.max_tokens,
      cache_retention: usesOpenAiOAuth ? 'off' : draft.api === 'anthropic' ? 'short' : draft.cache_retention,
      openrouter_targets: targets,
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
      setOpenRouterTargets(openRouterTargetsText(updated.openrouter_targets));
      setTestSucceeded(false);
      reload.remember({ provider: updated, keys });
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
      navigate({ type: 'show-settings-providers' });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The provider could not be deleted.'));
      setDeleting(false);
    }
  }

  const dirty = detail !== null && draft !== null && (
    draft.model !== detail.model
    || draft.reasoning_effort !== detail.reasoning_effort
    || draft.api !== detail.api
    || draft.auth !== detail.auth
    || draft.api_key !== (keys.some(({ id: keyId }) => keyId === detail.api_key)
      ? detail.api_key : null)
    || baseUrl !== providerBaseUrl(detail)
    || openRouterTargets !== openRouterTargetsText(detail.openrouter_targets)
  );
  reload.markDirty(dirty);

  function reset() {
    setError(null);
    setTestSucceeded(false);
    if (reload.stale) {
      reload.accept(applyProvider);
      return;
    }
    if (!detail) return;
    setDraft(providerDraft(detail, keys));
    setBaseUrl(providerBaseUrl(detail));
    setOpenRouterTargets(openRouterTargetsText(detail.openrouter_targets));
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="Provider settings">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-providers' })} type="button">
        <ChevronLeftIcon /><span>Providers</span>
      </button>
      {(detail || state.inspectedProvider.name) && <div className="cha-detail-actions">
        {detail ? <DetailRefreshProvider value={reload.context}><EditableTitle
          available={detail.writable} disabled={saving || deleting}
          id={detail.id} name={detail.display_name} subject="Provider"
          onSave={async (display_name) => {
            setSaving(true);
            try {
              const saved = await client.updateProvider(detail.id, { ...providerUpdate(detail), display_name });
              setDetail(saved);
              setDraft((current) => current && ({ ...current, display_name: saved.display_name }));
              reload.remember({ provider: saved, keys });
              dispatch({ type: 'provider-updated', providerId: saved.id,
                providerName: saved.display_name, writable: saved.writable });
            } finally {
              setSaving(false);
            }
          }}
        /></DetailRefreshProvider> : <h1>{state.inspectedProvider.name}</h1>}
      </div>}
      {!id && <p className="cha-state-message">No provider is selected.</p>}
      {id && !detail && !error && <p className="cha-state-message" role="status">Loading provider…</p>}
      {error && (!detail || reload.refreshFailed) && <LoadFailure message={error} retry={reload.retry} />}
      {detail && draft && (
        <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
          <fieldset aria-label="Provider details" disabled={saving || testing || deleting || !detail.writable}>
            <label>Model<input className="cha-form-control" onChange={(event) => change('model', event.target.value)} value={draft.model} /></label>
            {draft.api !== 'google' && draft.api !== 'anthropic' && <label>Reasoning effort
              <select className="cha-form-control" onChange={(event) => change('reasoning_effort', event.target.value as ProviderUpdate['reasoning_effort'])} value={draft.reasoning_effort}>
                <option value="none">None</option>
                <option value="low">Low</option>
                <option value="medium">Medium</option>
                <option value="high">High</option>
                <option value="xhigh">Extra high</option>
              </select>
            </label>}
            <label>Base URL<input className="cha-form-control" onChange={(event) => { setBaseUrl(event.target.value); setError(null); setTestSucceeded(false); }} placeholder="https://api.openai.com" type="url" value={baseUrl} /></label>
            <div className="cha-settings-form-grid">
              <label>API format<select className="cha-form-control" onChange={(event) => change('api', event.target.value as ProviderUpdate['api'])} value={draft.api}><option value="responses">Responses</option><option value="chat_completions">Chat completions</option><option value="google">Google (Gemini)</option><option value="anthropic">Anthropic</option></select></label>
              <label>Credentials<select className="cha-form-control" onChange={(event) => changeCredential(event.target.value)} value={draft.auth === 'openai_subscription'
                ? openAiOAuthCredential : draft.api_key ?? ''}><option value="">No credentials</option>{draft.api !== 'google' && draft.api !== 'anthropic' && <option value={openAiOAuthCredential}>OpenAI OAuth</option>}{keys.map((key) => <option key={key.id} value={key.id}>{key.display_name}</option>)}</select></label>
            </div>
            {draft.api !== 'google' && draft.api !== 'anthropic' && isOpenRouterBaseUrl(baseUrl) && <label>Inference targets<textarea className="cha-form-control cha-provider-targets" onChange={(event) => { setOpenRouterTargets(event.target.value); setError(null); setTestSucceeded(false); }} rows={3} value={openRouterTargets} /></label>}
          </fieldset>
          {!detail.writable && <p>This provider is read-only.</p>}
          <UsedBy empty="No characters use this provider." items={detail.used_by} />
          {error && !reload.refreshFailed && <p className="cha-error-message" role="alert">{error}</p>}
          {reload.stale && <StaleNotice onReload={() => reload.accept(applyProvider)} />}
          {testSucceeded && <p className="cha-settings-saved" role="status"><span aria-hidden="true" className="cha-settings-status-marker" /> Provider responded successfully.</p>}
          <div className="cha-settings-form-actions">
            <button className="cha-button" disabled={saving || testing || deleting} onClick={() => void runTest()} type="button">{testing ? 'Testing…' : 'Test'}</button>
            <button className="cha-button cha-button-ghost" disabled={(!dirty && !reload.stale) || saving || testing || deleting} onClick={reset} type="button">Cancel</button>
            <button className="cha-button cha-button-primary cha-provider-save-action" disabled={!dirty || saving || testing || deleting || !detail.writable || reload.blocked} type="submit">{saving ? 'Saving…' : 'Save changes'}</button>
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
