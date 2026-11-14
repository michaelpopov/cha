import { useEffect, useRef, useState, type FormEvent } from 'react';
import { publicErrorMessage, type NotesSettings, type ApiKeyDetail, type ProviderSummary } from '../api/client';
import { StaleNotice, useFormReload } from '../detailRefresh';
import { BackToSettings, LoadFailure, type SettingsScreenProps } from './SettingsShared';

export function NotesSettingsScreen({ client, navigate }: SettingsScreenProps) {
  const [settings, setSettings] = useState<NotesSettings | null>(null);
  const [saved, setSaved] = useState<NotesSettings | null>(null);
  const [providers, setProviders] = useState<ProviderSummary[]>([]);
  const [keys, setKeys] = useState<ApiKeyDetail[]>([]);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [testing, setTesting] = useState(false);
  const [testPassed, setTestPassed] = useState(false);
  // A provider change, a vault switch, or leaving the panel starts a new number,
  // so a late Flex test reply has no effect.
  const testRun = useRef(0);
  const reload = useFormReload('notes-settings');

  function applySettings(value: { config: NotesSettings; providers: ProviderSummary[] }) {
    setSaved(value.config);
    setSettings(value.config);
    setProviders(value.providers);
  }

  useEffect(() => {
    let current = true;
    const background = reload.start();
    if (!background) setError(null);
    void Promise.all([client.getNotesSettings(), client.listProviders(), client.listApiKeys()]).then(
      ([config, loadedProviders, loadedKeys]) => {
        if (!current) return;
        setKeys(loadedKeys);
        setError(null);
        reload.loaded({ config, providers: loadedProviders }, applySettings);
      },
      (failure: unknown) => {
        if (!current) return;
        setError(publicErrorMessage(failure, 'Notes settings could not be loaded.'));
        reload.fail(background);
      },
    );
    return () => { current = false; };
  }, [client, reload.epoch, reload.attempt]);

  useEffect(() => {
    setTesting(false);
    setTestPassed(false);
    return () => { testRun.current += 1; };
  }, [reload.epoch]);

  const dirty = settings !== null && saved !== null
    && (settings.compression_provider !== saved.compression_provider
      || settings.compression_reasoning_effort !== saved.compression_reasoning_effort
      || settings.audio_url !== saved.audio_url || settings.audio_api_key !== saved.audio_api_key);
  reload.markDirty(dirty);

  async function save(event: FormEvent) {
    event.preventDefault();
    if (!settings || !dirty || pending || reload.blocked) return;
    setPending(true);
    setError(null);
    try {
      const next = await client.saveNotesSettings(settings);
      setSaved(next);
      setSettings(next);
      reload.remember({ config: next, providers });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Notes settings could not be saved.'));
    } finally { setPending(false); }
  }

  async function testFlex() {
    if (!settings?.compression_provider || testing) return;
    const run = ++testRun.current;
    setTesting(true);
    setTestPassed(false);
    setError(null);
    try {
      await client.testNotesFlex(settings.compression_provider);
      if (run === testRun.current) setTestPassed(true);
    } catch (failure: unknown) {
      if (run === testRun.current) setError(publicErrorMessage(failure, 'Flex test failed.'));
    } finally {
      if (run === testRun.current) setTesting(false);
    }
  }

  return <section className="cha-screen cha-navigation" aria-label="Notes settings">
    <BackToSettings navigate={navigate} />
    {!settings && !error && <p className="cha-state-message" role="status">Loading notes settings…</p>}
    {error && (!settings || reload.refreshFailed)
      ? <LoadFailure message={error} retry={reload.retry} />
      : error && <p className="cha-error-message" role="alert">{error}</p>}
    {reload.stale && <StaleNotice onReload={() => reload.accept(applySettings)} />}
    {settings && <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
      <label>Compression provider<select className="cha-form-control" disabled={pending}
        value={settings.compression_provider}
        onChange={(event) => {
          testRun.current += 1;
          setTesting(false);
          setTestPassed(false);
          setError(null);
          setSettings({ ...settings, compression_provider: event.target.value });
        }}>
        <option value="">Select provider</option>
        {providers.map((provider) => <option key={provider.id} value={provider.id}>{provider.display_name}</option>)}
      </select></label>
      <label>Effort<select className="cha-form-control" disabled={pending}
        value={settings.compression_reasoning_effort}
        onChange={(event) => setSettings({ ...settings, compression_reasoning_effort: event.target.value as NotesSettings['compression_reasoning_effort'] })}>
        <option value="none">None</option><option value="low">Low</option><option value="medium">Medium</option>
        <option value="high">High</option><option value="xhigh">Extra high</option>
      </select></label>
      {client.noteAudio && <>
        <hr />
        <label>Audio service URL<input className="cha-form-control" type="url" disabled={pending}
          value={settings.audio_url ?? ''} onChange={event => setSettings({ ...settings, audio_url: event.target.value })} /></label>
        <label>Audio API key<select className="cha-form-control" disabled={pending}
          value={settings.audio_api_key ?? ''} onChange={event => setSettings({ ...settings, audio_api_key: event.target.value })}>
          <option value="">Select API key</option>
          {keys.map(key => <option key={key.id} value={key.id}>{key.display_name}</option>)}
        </select></label>
      </>}
      {testPassed && <p className="cha-settings-saved" role="status"><span aria-hidden="true"
        className="cha-settings-status-marker" /> Flex test passed.</p>}
      <div className="cha-settings-form-actions">
        <button className="cha-button" type="button" disabled={!settings.compression_provider || testing}
          onClick={() => void testFlex()}>{testing ? 'Testing…' : 'Test Flex API'}</button>
        <button className="cha-button cha-button-primary" type="submit"
          disabled={!dirty || pending || reload.blocked}>Save</button>
      </div>
    </form>}
  </section>;
}
