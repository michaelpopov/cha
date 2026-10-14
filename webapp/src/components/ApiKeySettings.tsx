import { useEffect, useState, type FormEvent } from 'react';
import { publicErrorMessage, type ApiKeyDetail, type ChaClient } from '../api/client';
import { DetailRefreshProvider, StaleNotice, useFormReload } from '../detailRefresh';
import { useLoad } from '../useLoad';
import { ConfirmDialog } from './ConfirmDialog';
import { EditableTitle } from './DetailActions';
import {
  ChevronLeftIcon,
  DatabaseIcon,
  KeyIcon,
  PlusIcon,
} from './Icons';
import { TransliteratingInput } from './TransliterationMode';
import {
  BackToSettings,
  LoadFailure,
  SettingsRow,
  UsedBy,
  type SettingsScreenProps,
} from './SettingsShared';

const loadApiKeys = (client: ChaClient) => client.listApiKeys();

export function ApiKeysScreen({ client, navigate }: SettingsScreenProps) {
  const { data: keys, error, retry } = useLoad(
    client, loadApiKeys, 'API keys could not be loaded.',
  );
  return (
    <section className="cha-screen cha-navigation" aria-label="API keys settings">
      <BackToSettings navigate={navigate} />
      {keys === null && !error && <p className="cha-state-message" role="status">Loading API keys…</p>}
      {error && <LoadFailure message={error} retry={retry} />}
      {keys && <div className="cha-list"><SettingsRow description="Configure database upload and download" icon={<DatabaseIcon />} label="R2 storage" onClick={() => navigate({ type: 'show-settings-r2-storage' })} /><SettingsRow description="Save a model-service secret in this vault" icon={<PlusIcon />} label="New API key" onClick={() => navigate({ type: 'show-settings-new-api-key' })} />{keys.length === 0 && <p className="cha-empty-list">No model API keys saved</p>}{keys.map((key) => <SettingsRow description={key.used_by.length ? `Used by ${key.used_by.join(', ')}` : 'Saved in this vault · Not in use'} icon={<KeyIcon />} key={key.id} label={key.display_name} onClick={() => navigate({ type: 'inspect-api-key', apiKeyId: key.id, apiKeyName: key.display_name })} />)}</div>}
    </section>
  );
}

export function NewApiKeyScreen({ client, navigate }: SettingsScreenProps) {
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
      navigate({ type: 'inspect-api-key', apiKeyId: created.id, apiKeyName: created.display_name });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The API key could not be saved.'));
      setSaving(false);
    }
  }
  return (
    <section className="cha-screen cha-navigation" aria-label="New API key settings">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-api-keys' })} type="button"><ChevronLeftIcon /><span>API Keys</span></button>
      <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
        <fieldset disabled={saving}><TransliteratingInput autoFocus autoComplete="off" className="cha-form-control" id="cha-new-api-key-name" label="Name" onValueChange={setName} placeholder="e.g. OpenRouter" value={name} /><label>API key<input autoComplete="off" className="cha-form-control" onChange={(event) => setValue(event.target.value)} placeholder="Paste key" type="password" value={value} /></label></fieldset>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving} onClick={() => navigate({ type: 'show-settings-api-keys' })} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!name.trim() || !value || saving} type="submit">{saving ? 'Saving…' : 'Save API key'}</button></div>
      </form>
    </section>
  );
}

export function ApiKeyScreen({ client, dispatch, navigate, state }: SettingsScreenProps) {
  const id = state.inspectedApiKey.id;
  const [key, setKey] = useState<ApiKeyDetail | null>(null);
  const [replacement, setReplacement] = useState('');
  const [busy, setBusy] = useState<'name' | 'value' | 'delete' | null>(null);
  const [confirming, setConfirming] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const reload = useFormReload(id);

  function applyKey(loaded: ApiKeyDetail) {
    setKey(loaded);
    setReplacement('');
    dispatch({
      type: 'api-key-detail-loaded',
      apiKeyId: loaded.id,
      apiKeyName: loaded.display_name,
    });
  }

  useEffect(() => {
    let current = true;
    const background = reload.start();
    if (!background) {
      setKey(null);
      setReplacement('');
      setError(null);
    }
    if (!id) return () => { current = false; };
    void client.listApiKeys().then(
      (keys) => {
        if (!current) return;
        const loaded = keys.find((candidate) => candidate.id === id);
        if (!loaded) {
          setError('That API key was not found.');
          reload.fail(background);
          return;
        }
        setError(null);
        reload.loaded(loaded, applyKey);
      },
      (failure: unknown) => {
        if (!current) return;
        setError(publicErrorMessage(failure, 'The API key could not be loaded.'));
        reload.fail(background);
      },
    );
    return () => { current = false; };
  }, [client, dispatch, id, reload.epoch, reload.attempt]);

  async function replace(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!id || !replacement || busy) return;
    setBusy('value'); setError(null);
    try {
      const updated = await client.replaceApiKeyValue(id, replacement);
      setKey(updated);
      setReplacement('');
      reload.remember(updated);
    }
    catch (failure: unknown) { setError(publicErrorMessage(failure, 'The API key value could not be replaced.')); }
    finally { setBusy(null); }
  }
  async function remove() {
    setConfirming(false);
    if (!id || busy) return;
    setBusy('delete'); setError(null);
    try { await client.deleteApiKey(id); navigate({ type: 'show-settings-api-keys' }); }
    catch (failure: unknown) { setError(publicErrorMessage(failure, 'The API key could not be removed.')); setBusy(null); }
  }
  reload.markDirty(replacement !== '');
  return (
    <section className="cha-screen cha-navigation" aria-label="API key settings">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-api-keys' })} type="button"><ChevronLeftIcon /><span>API Keys</span></button>
      {(key || state.inspectedApiKey.name) && <div className="cha-detail-actions">
        {key ? <DetailRefreshProvider value={reload.context}><EditableTitle
          available disabled={busy !== null} id={key.id} name={key.display_name} subject="API key"
          onSave={async (displayName) => {
            setBusy('name');
            try {
              const saved = await client.renameApiKey(key.id, displayName);
              setKey(saved);
              reload.remember(saved);
              dispatch({ type: 'api-key-updated', apiKeyId: saved.id, apiKeyName: saved.display_name });
            } finally {
              setBusy(null);
            }
          }}
        /></DetailRefreshProvider> : <h1>{state.inspectedApiKey.name}</h1>}
      </div>}
      {!id && <p className="cha-state-message">No API key is selected.</p>}
      {id && !key && !error && <p className="cha-state-message" role="status">Loading API key…</p>}
      {error && (!key || reload.refreshFailed) && <LoadFailure message={error} retry={reload.retry} />}
      {key && <form className="cha-settings-form" onSubmit={(event) => void replace(event)}>
        <fieldset disabled={busy !== null}><label>New API key<input autoComplete="off" className="cha-form-control" onChange={(event) => setReplacement(event.target.value)} placeholder="Paste replacement key" type="password" value={replacement} /></label><div className="cha-settings-form-actions"><button className="cha-button cha-button-primary" disabled={!replacement || busy !== null || reload.blocked} type="submit">{busy === 'value' ? 'Saving…' : 'Save'}</button></div></fieldset>
        <UsedBy empty="Nothing references this key." items={key.used_by} />
        {error && !reload.refreshFailed && <p className="cha-error-message" role="alert">{error}</p>}
        {reload.stale && <StaleNotice onReload={() => reload.accept(applyKey)} />}
        <div className="cha-settings-form-actions">
          <button className="cha-button cha-button-danger" disabled={busy !== null} onClick={() => setConfirming(true)} type="button">{busy === 'delete' ? 'Removing…' : 'Remove API key'}</button>
        </div>
      </form>}
      {confirming && (
        <ConfirmDialog
          confirmLabel="Remove API key"
          message={`Remove “${state.inspectedApiKey.name ?? 'this key'}” from this vault? Features using it will stop authenticating.`}
          onCancel={() => setConfirming(false)}
          onConfirm={() => void remove()}
          title="Remove API key?"
        />
      )}
    </section>
  );
}
