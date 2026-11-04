import { useCallback, useEffect, useRef, useState } from 'react';
import { ChaError, publicErrorMessage, type ChaClient, type CloudStoreList } from '../api/client';
import { useLoad } from '../useLoad';
import { ConfirmDialog } from './ConfirmDialog';
import { ChevronLeftIcon, DatabaseIcon, KeyIcon } from './Icons';
import { BackToSettings, LoadFailure, SettingsRow, type SettingsScreenProps } from './SettingsShared';

const loadApiKeys = (client: ChaClient) => client.listApiKeys();
const createdFormat = new Intl.DateTimeFormat(undefined, { dateStyle: 'medium', timeStyle: 'short' });

function formatCreated(text: string): string {
  const date = new Date(text);
  return Number.isNaN(date.getTime()) ? text : createdFormat.format(date);
}

// A row shows the store name, or its ID when it has none. A named row whose name
// matches another row's label also shows its ID.
export function cloudStoreLabels(items: CloudStoreList['items']): string[] {
  const labels = items.map((item) => item.name || item.id);
  return items.map((item, index) => (
    item.name && labels.some((label, other) => other !== index && label === item.name)
      ? `${item.name} (${item.id})` : labels[index]));
}

export function CloudStoreKeysScreen({ client, navigate }: SettingsScreenProps) {
  const { data, error, retry } = useLoad(client, loadApiKeys, 'API keys could not be loaded.');
  const keys = data?.filter((key) => key.cloud_store !== 'none') ?? null;
  return (
    <section className="cha-screen cha-navigation" aria-label="Cloud stores settings">
      <BackToSettings navigate={navigate} />
      {keys === null && !error && <p className="cha-state-message" role="status">Loading API keys…</p>}
      {error && <LoadFailure message={error} retry={retry} />}
      {keys && <div className="cha-list">
        {keys.length === 0 && <p className="cha-empty-list">No cloud store keys</p>}
        {keys.map((key) => <SettingsRow icon={<KeyIcon />} key={key.id} label={key.display_name}
          onClick={() => navigate({ type: 'inspect-cloud-store-key', apiKeyId: key.id, apiKeyName: key.display_name })} />)}
      </div>}
    </section>
  );
}

export function CloudStoresScreen({ client, navigate, state }: SettingsScreenProps) {
  const { id: apiKeyId, notice } = state.inspectedCloudStoreKey;
  const load = useCallback((loader: ChaClient) => (
    apiKeyId ? loader.listCloudStores(apiKeyId) : Promise.reject(new Error('No API key'))), [apiKeyId]);
  const { data, error, retry } = useLoad(client, load, 'Cloud stores could not be loaded.');
  const labels = data ? cloudStoreLabels(data.items) : [];
  return (
    <section className="cha-screen cha-navigation" aria-label="Cloud store list">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-cloud-stores' })} type="button">
        <ChevronLeftIcon /><span>Cloud stores</span>
      </button>
      {notice && <p className="cha-error-message" role="alert">{notice}</p>}
      {data === null && !error && <p className="cha-state-message" role="status">Loading cloud stores…</p>}
      {error && <LoadFailure message={error} retry={retry} />}
      {data && <div className="cha-list">
        {data.items.length === 0 && <p className="cha-empty-list">No cloud stores</p>}
        {data.items.map((store, index) => <SettingsRow icon={<DatabaseIcon />} key={store.id} label={labels[index]}
          onClick={() => navigate({ type: 'inspect-cloud-store', storeId: store.id, label: labels[index] })} />)}
      </div>}
    </section>
  );
}

export function CloudStoreScreen({ client, navigate, state }: SettingsScreenProps) {
  const { id: apiKeyId, name: apiKeyName } = state.inspectedCloudStoreKey;
  const { id: storeId, label } = state.inspectedCloudStore;
  const load = useCallback((loader: ChaClient) => (
    apiKeyId && storeId ? loader.getCloudStore(apiKeyId, storeId) : Promise.reject(new Error('No store'))),
  [apiKeyId, storeId]);
  const { data: store, error: loadError, retry } = useLoad(client, load, 'The cloud store could not be loaded.');
  const [confirming, setConfirming] = useState(false);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const deletion = useRef<AbortController | null>(null);
  // Replies for a deletion that was cancelled or left are ignored.
  useEffect(() => () => { deletion.current = null; }, []);

  function backToList(notice?: string) {
    if (apiKeyId) navigate({ type: 'inspect-cloud-store-key', apiKeyId, apiKeyName: apiKeyName ?? '', notice });
    else navigate({ type: 'show-settings-cloud-stores' });
  }
  async function confirmDelete() {
    if (!apiKeyId || !storeId || deletion.current) return;
    const controller = new AbortController();
    deletion.current = controller;
    setPending(true);
    setError(null);
    try {
      await client.deleteCloudStore(apiKeyId, storeId, controller.signal);
      if (deletion.current !== controller) return;
      deletion.current = null;
      backToList();
    } catch (failure: unknown) {
      if (deletion.current !== controller) return;
      deletion.current = null;
      setPending(false);
      setConfirming(false);
      // The store is gone, so its details cannot be retried.
      if (failure instanceof ChaError && failure.code === 'not_found') backToList(failure.message);
      else setError(publicErrorMessage(failure, 'The store could not be deleted.'));
    }
  }
  function cancelDelete() {
    const controller = deletion.current;
    setConfirming(false);
    if (!controller) return;
    deletion.current = null;
    controller.abort();
    setPending(false);
    setError('Deletion stopped. Some files may already be deleted.');
  }

  const openai = !storeId?.startsWith('fileSearchStores/');
  const message = `Permanently delete “${label ?? storeId ?? ''}” and its files?`
    + (openai ? ' Files shared with other vector stores are removed from them too.' : '');
  return (
    <section className="cha-screen cha-navigation" aria-label="Cloud store details">
      <button className="cha-back-row" onClick={() => backToList()} type="button">
        <ChevronLeftIcon /><span>{apiKeyName}</span>
      </button>
      {store === null && !loadError && <p className="cha-state-message" role="status">Loading cloud store…</p>}
      {loadError && <LoadFailure message={loadError} retry={retry} />}
      {store && <div className="cha-settings-form">
        <label>Name<input className="cha-form-control" readOnly value={store.name} /></label>
        <label>Description<textarea className="cha-form-control" readOnly rows={6} value={store.description} /></label>
        <label>Created<input className="cha-form-control" readOnly value={formatCreated(store.created_at)} /></label>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions">
          <button className="cha-button cha-button-danger" disabled={pending} onClick={() => setConfirming(true)} type="button">Delete</button>
        </div>
      </div>}
      {confirming && (
        <ConfirmDialog
          confirmLabel={pending ? 'Deleting…' : 'Delete store'}
          message={message}
          onCancel={cancelDelete}
          onConfirm={() => void confirmDelete()}
          pending={pending}
          title="Delete store?"
        />
      )}
    </section>
  );
}
