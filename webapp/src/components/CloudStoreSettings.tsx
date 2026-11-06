import { useCallback, useEffect, useRef, useState, type FormEvent } from 'react';
import { ChaError, publicErrorMessage, type ChaClient, type CloudStoreDetail, type CloudStoreList, type CloudStoreUpdate } from '../api/client';
import { useDetailRefresh, useFormReload } from '../detailRefresh';
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
  const { id: apiKeyId, name: apiKeyName, notice } = state.inspectedCloudStoreKey;
  const { epoch } = useDetailRefresh();
  const [refreshing, setRefreshing] = useState(false);
  const [refreshError, setRefreshError] = useState<string | null>(null);
  const refreshRequest = useRef<object | null>(null);
  const load = useCallback((loader: ChaClient) => (
    apiKeyId ? loader.listCloudStores(apiKeyId) : Promise.reject(new Error('No API key'))), [apiKeyId]);
  const { data, error, retry, setData } = useLoad(client, load, 'Cloud stores could not be loaded.');
  useEffect(() => {
    setRefreshing(false);
    setRefreshError(null);
    return () => { refreshRequest.current = null; };
  }, [client, apiKeyId, epoch]);
  async function refresh() {
    if (!apiKeyId || refreshRequest.current) return;
    const request = {};
    refreshRequest.current = request;
    setRefreshing(true);
    setRefreshError(null);
    try {
      const loaded = await client.refreshCloudStores(apiKeyId);
      if (refreshRequest.current === request) setData(loaded);
    } catch (failure: unknown) {
      if (refreshRequest.current === request)
        setRefreshError(publicErrorMessage(failure, 'Cloud stores could not be refreshed.'));
    } finally {
      if (refreshRequest.current === request) {
        refreshRequest.current = null;
        setRefreshing(false);
      }
    }
  }
  const labels = data ? cloudStoreLabels(data.items) : [];
  return (
    <section className="cha-screen cha-navigation" aria-label="Cloud store list">
      <div className="cha-detail-actions">
        <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-cloud-stores' })} type="button">
          <ChevronLeftIcon /><span>Cloud stores{apiKeyName ? ` · ${apiKeyName}` : ''}</span>
        </button>
        <button className="cha-button" disabled={refreshing || !apiKeyId} onClick={() => void refresh()} type="button">
          {refreshing ? 'Refreshing…' : 'Refresh'}
        </button>
      </div>
      {notice && <p className="cha-error-message" role="alert">{notice}</p>}
      {refreshError && <p className="cha-error-message" role="alert">{refreshError}</p>}
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
  const [store, setStore] = useState<CloudStoreDetail | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);
  const reload = useFormReload(JSON.stringify([apiKeyId, storeId]));
  const [draft, setDraft] = useState<CloudStoreUpdate | null>(null);
  const [saving, setSaving] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const deletion = useRef<AbortController | null>(null);
  const saveRequest = useRef<object | null>(null);
  function applyStore(loaded: CloudStoreDetail) {
    setStore(loaded);
    setDraft(null);
  }
  useEffect(() => {
    let current = true;
    const background = reload.start();
    if (!background) { setStore(null); setDraft(null); }
    setLoadError(null);
    if (!apiKeyId || !storeId) return () => { current = false; };
    void client.getCloudStore(apiKeyId, storeId).then(
      (loaded) => {
        if (current) reload.loaded(loaded, applyStore);
      },
      (failure: unknown) => {
        if (!current) return;
        setLoadError(publicErrorMessage(failure, 'The cloud store could not be loaded.'));
        reload.fail(background);
      },
    );
    return () => { current = false; };
  }, [client, apiKeyId, storeId, reload.epoch, reload.attempt]);
  reload.markDirty(draft !== null && store !== null
    && (draft.name !== store.name || draft.description !== store.description));
  useEffect(() => {
    setDraft(null);
    setSaving(false);
    setError(null);
    setPending(false);
    setConfirming(false);
    return () => { saveRequest.current = null; deletion.current = null; };
  }, [apiKeyId, storeId]);

  function backToList(notice?: string) {
    if (apiKeyId) navigate({ type: 'inspect-cloud-store-key', apiKeyId, apiKeyName: apiKeyName ?? '', notice });
    else navigate({ type: 'show-settings-cloud-stores' });
  }
  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!apiKeyId || !storeId || !draft?.name.trim() || saveRequest.current || pending || reload.blocked || loadError) return;
    setSaving(true);
    setError(null);
    const request = {};
    saveRequest.current = request;
    try {
      const saved = await client.updateCloudStore(apiKeyId, storeId, { name: draft.name.trim(), description: draft.description });
      if (saveRequest.current !== request) return;
      applyStore(saved);
      reload.remember(saved);
    } catch (failure: unknown) {
      if (saveRequest.current === request) setError(publicErrorMessage(failure, 'The store could not be saved.'));
    } finally {
      if (saveRequest.current === request) {
        saveRequest.current = null;
        setSaving(false);
      }
    }
  }
  async function confirmDelete() {
    if (!apiKeyId || !storeId || deletion.current || saving) return;
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
  const currentLabel = store && store.name !== label && `${store.name} (${storeId})` !== label
    ? store.name || storeId : label;
  const message = `Permanently delete “${currentLabel ?? storeId ?? ''}” and its files?`
    + (openai ? ' Files shared with other vector stores are removed from them too.' : '');
  return (
    <section className="cha-screen cha-navigation" aria-label="Cloud store details">
      <button className="cha-back-row" onClick={() => backToList()} type="button">
        <ChevronLeftIcon /><span>{apiKeyName}</span>
      </button>
      {store === null && !loadError && <p className="cha-state-message" role="status">Loading cloud store…</p>}
      {(loadError || reload.stale) && <LoadFailure
        message={loadError ?? 'The cloud store changed. Try again to load its saved values.'}
        retry={reload.stale ? () => reload.accept(applyStore) : reload.retry} />}
      {store && <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
        <label>Name<input className="cha-form-control" disabled={saving || pending} required
          onChange={(event) => setDraft({ name: event.target.value, description: draft?.description ?? store.description })}
          value={draft?.name ?? store.name} /></label>
        <label>Description<textarea className="cha-form-control" disabled={saving || pending} rows={6}
          onChange={(event) => setDraft({ name: draft?.name ?? store.name, description: event.target.value })}
          value={draft?.description ?? store.description} /></label>
        <label>Created<input className="cha-form-control" readOnly value={formatCreated(store.created_at)} /></label>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions">
          <button className="cha-button cha-button-primary" disabled={saving || pending || reload.blocked || !!loadError || !draft?.name.trim()}
            type="submit">{saving ? 'Saving…' : 'Save'}</button>
        </div>
        <div className="cha-settings-form-actions">
          <button className="cha-button cha-button-danger" disabled={pending || saving} onClick={() => setConfirming(true)} type="button">Delete</button>
        </div>
      </form>}
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
