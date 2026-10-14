import { useEffect, useState, type FormEvent } from 'react';
import { publicErrorMessage, type ApiKeyDetail, type WebSearchSettings } from '../api/client';
import { StaleNotice, useFormReload } from '../detailRefresh';
import { BackToSettings, type SettingsScreenProps } from './SettingsShared';

const defaultWebSearch: WebSearchSettings = {
  provider: 'brave', api_key: '', tool_enabled: false,
  read_provider: 'off', firecrawl_api_key: '', web_reader_url: '', web_reader_enabled: false, web_reader_api_key: '',
};

export function WebSearchSettingsScreen({ client, navigate }: SettingsScreenProps) {
  const [settings, setSettings] = useState<WebSearchSettings>(defaultWebSearch);
  const [saved, setSaved] = useState<WebSearchSettings | null>(null);
  const [keys, setKeys] = useState<ApiKeyDetail[] | null>(null);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const reload = useFormReload('web-search');

  function applySearch(value: { config: WebSearchSettings; keys: ApiKeyDetail[] }) {
    setSaved(value.config);
    setSettings(value.config);
    setKeys(value.keys);
  }

  useEffect(() => {
    let current = true;
    const background = reload.start();
    if (!background) setError(null);
    void Promise.all([
      client.getWebSearchSettings(), client.listApiKeys(),
    ]).then(
      ([config, apiKeys]) => {
        if (!current) return;
        setError(null);
        reload.loaded({ config, keys: apiKeys }, applySearch);
      },
      (failure: unknown) => {
        if (!current) return;
        setError(publicErrorMessage(failure, 'Search API settings could not be loaded.'));
        reload.fail(background);
      },
    );
    return () => { current = false; };
  }, [client, reload.epoch, reload.attempt]);

  const dirty = saved !== null && (
    settings.read_provider !== saved.read_provider
    || settings.firecrawl_api_key !== saved.firecrawl_api_key
    || settings.web_reader_url !== saved.web_reader_url
    || settings.web_reader_enabled !== saved.web_reader_enabled
    || settings.web_reader_api_key !== saved.web_reader_api_key
    || settings.tool_enabled !== saved.tool_enabled
    || settings.provider !== saved.provider
    || settings.api_key !== saved.api_key
  );
  reload.markDirty(dirty);

  async function save(event: FormEvent) {
    event.preventDefault();
    if (!dirty || pending) return;
    setPending(true); setError(null);
    try {
      const next = await client.saveWebSearchSettings(settings);
      setSaved(next); setSettings(next);
      reload.remember({ config: next, keys: keys ?? [] });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Search API settings could not be saved.'));
    } finally { setPending(false); }
  }

  return <section className="cha-screen cha-navigation" aria-label="Search API">
    <BackToSettings navigate={navigate} />
    {error && <p className="cha-error-message" role="alert">{error}</p>}
    {reload.refreshFailed && <button className="cha-button cha-button-ghost" onClick={reload.retry} type="button">Try again</button>}
    {reload.stale && <StaleNotice onReload={() => reload.accept(applySearch)} />}
    {keys && <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
      <label className="cha-checkbox-row"><input checked={settings.tool_enabled} disabled={pending}
        onChange={(event) => setSettings({ ...settings, tool_enabled: event.target.checked })}
        type="checkbox" />On-demand web search</label>
      <label>API provider<select className="cha-form-control" disabled={pending}
        value={settings.provider}
        onChange={(event) => setSettings({ ...settings, provider: event.target.value as WebSearchSettings['provider'] })}>
        <option value="brave">Brave Search API</option>
        <option value="tavily">Tavily</option>
      </select></label>
      <label>API key<select className="cha-form-control" disabled={pending}
        value={settings.api_key}
        onChange={(event) => setSettings({ ...settings, api_key: event.target.value })}>
        <option value="">Select API key</option>
        {keys.map((key) => <option key={key.id} value={key.id}>{key.display_name}</option>)}
      </select></label>
      <hr className="cha-settings-divider" />
      <div className="cha-reader-settings">
        <label>Page reading provider<select className="cha-form-control" disabled={pending}
          value={settings.read_provider}
          onChange={(event) => setSettings({ ...settings,
            read_provider: event.target.value as WebSearchSettings['read_provider'] })}>
          <option value="off">Off</option>
          <option value="firecrawl">Firecrawl</option>
        </select></label>
        {settings.read_provider === 'firecrawl' && <label>Firecrawl API key<select className="cha-form-control" disabled={pending}
          value={settings.firecrawl_api_key}
          onChange={(event) => setSettings({ ...settings, firecrawl_api_key: event.target.value })}>
          <option value="">Select API key</option>
          {keys.map((key) => <option key={key.id} value={key.id}>{key.display_name}</option>)}
        </select></label>}
      </div>
      <div className="cha-reader-settings">
        <label className="cha-checkbox-row"><input checked={settings.web_reader_enabled} disabled={pending}
          onChange={(event) => setSettings({ ...settings, web_reader_enabled: event.target.checked })}
          type="checkbox" />Use web_reader</label>
        <label>web_reader URL<input className="cha-form-control" type="url" disabled={pending || !settings.web_reader_enabled}
          value={settings.web_reader_url} placeholder="http://192.168.86.39:8087"
          onChange={(event) => setSettings({ ...settings, web_reader_url: event.target.value })} /></label>
        <label>web_reader API key<select className="cha-form-control" disabled={pending || !settings.web_reader_enabled}
          value={settings.web_reader_api_key}
          onChange={(event) => setSettings({ ...settings, web_reader_api_key: event.target.value })}>
          <option value="">None</option>
          {keys.map((key) => <option key={key.id} value={key.id}>{key.display_name}</option>)}
        </select></label>
      </div>
      <div className="cha-settings-form-actions">
        <button className="cha-button cha-button-primary" type="submit"
          disabled={!dirty || pending || reload.blocked}>
          Save
        </button>
      </div>
    </form>}
  </section>;
}
