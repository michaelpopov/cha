import { useEffect, useState, type FormEvent } from 'react';
import {
  publicErrorMessage,
  type ApiKeyDetail,
  type JevSettings,
  type ProviderSummary,
  type SessionNamingSettings,
} from '../api/client';
import { StaleNotice, useFormReload } from '../detailRefresh';
import { ChevronLeftIcon } from './Icons';
import { type SettingsScreenProps } from './SettingsShared';

const defaultJev: JevSettings = {
  url: 'https://openrouter.ai/api/alpha/decisions', model: 'typesafe/jev-1.13', api_key: '',
};

export function SessionSettingsScreen({ client, navigate }: SettingsScreenProps) {
  const [settings, setSettings] = useState<JevSettings>(defaultJev);
  const [saved, setSaved] = useState<JevSettings | null>(null);
  const [naming, setNaming] = useState<SessionNamingSettings | null>(null);
  const [namingBaseline, setNamingBaseline] = useState<SessionNamingSettings | null>(null);
  const [providers, setProviders] = useState<ProviderSummary[] | null>(null);
  const [keys, setKeys] = useState<ApiKeyDetail[] | null>(null);
  const [pending, setPending] = useState(false);
  const [namingPending, setNamingPending] = useState(false);
  const [jevError, setJevError] = useState<string | null>(null);
  const [namingError, setNamingError] = useState<string | null>(null);
  const jevReload = useFormReload('jev');
  const namingReload = useFormReload('naming');

  function applyJev(value: { config: JevSettings | null; keys: ApiKeyDetail[] }) {
    setSaved(value.config);
    setSettings(value.config ?? defaultJev);
    setKeys(value.keys);
  }

  function applyNaming(value: { config: SessionNamingSettings; providers: ProviderSummary[] }) {
    setNamingBaseline(value.config);
    setNaming(value.config);
    setProviders(value.providers);
  }

  useEffect(() => {
    let current = true;
    const background = jevReload.start();
    if (!background) setJevError(null);
    void Promise.all([client.getJevSettings(), client.listApiKeys()]).then(([config, apiKeys]) => {
      if (!current) return;
      setJevError(null);
      jevReload.loaded({ config, keys: apiKeys }, applyJev);
    }, (failure: unknown) => {
      if (!current) return;
      setJevError(publicErrorMessage(failure, 'Recipient detection settings could not be loaded.'));
      jevReload.fail(background);
    });
    return () => { current = false; };
  }, [client, jevReload.epoch, jevReload.attempt]);

  useEffect(() => {
    let current = true;
    const background = namingReload.start();
    if (!background) setNamingError(null);
    void Promise.all([client.getSessionNamingSettings(), client.listProviders()]).then(([config, modelProviders]) => {
      if (!current) return;
      setNamingError(null);
      namingReload.loaded({ config, providers: modelProviders }, applyNaming);
    }, (failure: unknown) => {
      if (!current) return;
      setNamingError(publicErrorMessage(failure, 'Session naming settings could not be loaded.'));
      namingReload.fail(background);
    });
    return () => { current = false; };
  }, [client, namingReload.epoch, namingReload.attempt]);

  async function save(event: FormEvent) {
    event.preventDefault();
    setPending(true); setJevError(null);
    try {
      const next = await client.saveJevSettings({ ...settings, model: settings.model.trim(), url: settings.url.trim() });
      setSaved(next); setSettings(next);
      jevReload.remember({ config: next, keys: keys ?? [] });
    } catch (failure: unknown) {
      setJevError(publicErrorMessage(failure, 'Recipient detection settings could not be saved.'));
    } finally { setPending(false); }
  }
  async function disable() {
    setPending(true); setJevError(null);
    try {
      await client.disableJev();
      setSaved(null);
      setSettings(defaultJev);
      jevReload.remember({ config: null, keys: keys ?? [] });
    }
    catch (failure: unknown) { setJevError(publicErrorMessage(failure, 'Recipient detection could not be disabled.')); }
    finally { setPending(false); }
  }
  async function saveNaming(event: FormEvent) {
    event.preventDefault();
    if (!naming) return;
    setNamingPending(true); setNamingError(null);
    try {
      const next = await client.saveSessionNamingSettings(naming);
      setNamingBaseline(next);
      setNaming(next);
      namingReload.remember({ config: next, providers: providers ?? [] });
    } catch (failure: unknown) {
      setNamingError(publicErrorMessage(failure, 'Session naming settings could not be saved.'));
    } finally { setNamingPending(false); }
  }
  const jevDirty = saved === null
    ? settings.url !== defaultJev.url || settings.model !== defaultJev.model || settings.api_key !== ''
    : settings.url !== saved.url || settings.model !== saved.model || settings.api_key !== saved.api_key;
  const namingDirty = namingBaseline !== null && naming !== null && (
    naming.provider !== namingBaseline.provider
    || naming.reasoning_effort !== namingBaseline.reasoning_effort
  );
  jevReload.markDirty(jevDirty);
  namingReload.markDirty(namingDirty);
  return <section className="cha-screen cha-navigation" aria-label="Session settings">
    <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings' })} type="button"><ChevronLeftIcon /><span>Settings</span></button>
    {namingError && <p className="cha-error-message" role="alert">{namingError}</p>}
    {namingReload.refreshFailed && <button className="cha-button cha-button-ghost" onClick={namingReload.retry} type="button">Try again</button>}
    {namingReload.stale && <StaleNotice onReload={() => namingReload.accept(applyNaming)} />}
    {jevError && <p className="cha-error-message" role="alert">{jevError}</p>}
    {jevReload.refreshFailed && <button className="cha-button cha-button-ghost" onClick={jevReload.retry} type="button">Try again</button>}
    {jevReload.stale && <StaleNotice onReload={() => jevReload.accept(applyJev)} />}
    {naming && providers && <form className="cha-settings-form" onSubmit={(event) => void saveNaming(event)}>
      <h2 className="cha-settings-section-title">Session naming</h2>
      <label>Provider<select className="cha-form-control" disabled={namingPending}
        value={naming.provider} onChange={(event) => setNaming({ ...naming, provider: event.target.value })}>
        <option value="">Select provider</option>
        {providers.map((provider) => <option key={provider.id} value={provider.id}>{provider.display_name}</option>)}
      </select></label>
      <label>Effort<select className="cha-form-control" disabled={namingPending}
        value={naming.reasoning_effort} onChange={(event) => setNaming({ ...naming, reasoning_effort: event.target.value as SessionNamingSettings['reasoning_effort'] })}>
        <option value="none">None</option><option value="low">Low</option><option value="medium">Medium</option>
        <option value="high">High</option><option value="xhigh">Extra high</option>
      </select></label>
      <div className="cha-settings-form-actions"><button className="cha-button cha-button-primary"
        disabled={namingPending || namingReload.blocked || !providers.some((provider) => provider.id === naming.provider)}
        type="submit">Save session naming</button></div>
    </form>}
    {keys && <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
      <h2 className="cha-settings-section-title">Recipient detection</h2>
      <label>URL<input className="cha-form-control" type="url" required value={settings.url} disabled={pending} onChange={(event) => setSettings({ ...settings, url: event.target.value })} /></label>
      <label>Model<input className="cha-form-control" required value={settings.model} disabled={pending} onChange={(event) => setSettings({ ...settings, model: event.target.value })} /></label>
      <label>API key<select className="cha-form-control" required value={settings.api_key} disabled={pending} onChange={(event) => setSettings({ ...settings, api_key: event.target.value })}>
        <option value="">Select API key</option>
        {keys.map((key) => <option key={key.id} value={key.id}>{key.display_name}</option>)}
      </select></label>
      <div className="cha-settings-form-actions">
        <button className="cha-button cha-button-ghost" disabled={pending || !saved} onClick={() => void disable()} type="button">Disable recipient detection</button>
        <button className="cha-button cha-button-primary" disabled={pending || jevReload.blocked || !settings.url.trim() || !settings.model.trim() || !keys.some((key) => key.id === settings.api_key)} type="submit">Save recipient detection</button>
      </div>
    </form>}
  </section>;
}
