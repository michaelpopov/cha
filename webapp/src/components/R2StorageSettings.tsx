import { useEffect, useState, type FormEvent } from 'react';
import { publicErrorMessage, type R2StorageDetail } from '../api/client';
import { StaleNotice, useFormReload } from '../detailRefresh';
import { validateBootstrap } from '../state/bootstrap';
import { ConfirmDialog } from './ConfirmDialog';
import { ChevronLeftIcon } from './Icons';
import { LoadFailure, type SettingsScreenProps } from './SettingsShared';

interface R2Draft {
  url: string;
  access_key_id: string;
  secret_key: string;
}

function r2Draft(detail: R2StorageDetail | null): R2Draft {
  return {
    url: detail?.url ?? '',
    access_key_id: detail?.access_key_id ?? '',
    secret_key: '',
  };
}

export function R2StorageScreen({ client, dispatch, navigate }: SettingsScreenProps) {
  const [detail, setDetail] = useState<R2StorageDetail | null | undefined>(undefined);
  const [draft, setDraft] = useState<R2Draft>(() => r2Draft(null));
  const [busy, setBusy] = useState<'save' | 'delete' | null>(null);
  const [confirming, setConfirming] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const reload = useFormReload('r2');

  function applyR2(loaded: R2StorageDetail | null) {
    setDetail(loaded);
    setDraft(r2Draft(loaded));
  }

  async function refreshCapabilities() {
    try {
      dispatch({
        type: 'bootstrap-refreshed',
        bootstrap: validateBootstrap(await client.getBootstrap()),
      });
    } catch {
      // The credential change succeeded. A later bootstrap will refresh controls.
    }
  }

  useEffect(() => {
    let current = true;
    const background = reload.start();
    if (!background) {
      setDetail(undefined);
      setError(null);
    }
    void client.getR2Storage().then(
      (loaded) => {
        if (!current) return;
        setError(null);
        reload.loaded(loaded, applyR2);
      },
      (failure: unknown) => {
        if (!current) return;
        setError(publicErrorMessage(failure, 'R2 storage credentials could not be loaded.'));
        reload.fail(background);
      },
    );
    return () => { current = false; };
  }, [client, reload.epoch, reload.attempt]);

  function change(field: keyof R2Draft, value: string) {
    setDraft((current) => ({ ...current, [field]: value }));
  }

  const original = r2Draft(detail ?? null);
  const dirty = draft.url !== original.url
    || draft.access_key_id !== original.access_key_id
    || Boolean(draft.secret_key);
  const valid = Boolean(
    draft.url.trim()
    && draft.access_key_id.trim()
    && (detail || draft.secret_key),
  );
  reload.markDirty(dirty);
  function resetR2() {
    if (reload.stale) {
      reload.accept(applyR2);
      return;
    }
    setDraft(r2Draft(detail ?? null));
  }

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!valid || !dirty || busy) return;
    setBusy('save');
    setError(null);
    try {
      const saved = await client.saveR2Storage({
        display_name: detail?.display_name ?? 'R2',
        url: draft.url.trim(),
        access_key_id: draft.access_key_id.trim(),
        secret_key: draft.secret_key || null,
      });
      setDetail(saved);
      setDraft(r2Draft(saved));
      reload.remember(saved);
      await refreshCapabilities();
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'R2 storage credentials could not be saved.'));
    } finally {
      setBusy(null);
    }
  }

  async function remove() {
    setConfirming(false);
    if (!detail || busy) return;
    setBusy('delete');
    setError(null);
    try {
      await client.deleteR2Storage();
      await refreshCapabilities();
      navigate({ type: 'show-settings-api-keys' });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'R2 storage credentials could not be removed.'));
      setBusy(null);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="R2 storage settings">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-api-keys' })} type="button"><ChevronLeftIcon /><span>API Keys</span></button>
      {detail === undefined && !error && <p className="cha-state-message" role="status">Loading R2 storage credentials…</p>}
      {error && (detail === undefined || reload.refreshFailed) && <LoadFailure message={error} retry={reload.retry} />}
      {detail !== undefined && <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
        <fieldset disabled={busy !== null}>
          <label>R2 URL<input autoComplete="off" autoFocus className="cha-form-control" onChange={(event) => change('url', event.target.value)} placeholder="https://account.r2.cloudflarestorage.com/bucket" type="url" value={draft.url} /></label>
          <label>Access key ID<input autoComplete="off" className="cha-form-control" onChange={(event) => change('access_key_id', event.target.value)} placeholder="Paste access key ID" value={draft.access_key_id} /></label>
          <label>Secret key<input autoComplete="off" className="cha-form-control" onChange={(event) => change('secret_key', event.target.value)} placeholder={detail ? 'Leave blank to keep the current secret' : 'Paste secret key'} type="password" value={draft.secret_key} /></label>
        </fieldset>
        {error && !reload.refreshFailed && <p className="cha-error-message" role="alert">{error}</p>}
        {reload.stale && <StaleNotice onReload={() => reload.accept(applyR2)} />}
        <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={(!dirty && !reload.stale) || busy !== null} onClick={resetR2} type="button">Reset</button><button className="cha-button cha-button-primary" disabled={!dirty || !valid || busy !== null || reload.blocked} type="submit">{busy === 'save' ? 'Saving…' : 'Save R2 credentials'}</button></div>
        {detail && <div className="cha-settings-form-actions"><button className="cha-button cha-button-danger" disabled={busy !== null} onClick={() => setConfirming(true)} type="button">{busy === 'delete' ? 'Removing…' : 'Remove R2 credentials'}</button></div>}
      </form>}
      {confirming && <ConfirmDialog confirmLabel="Remove R2 credentials" message="Remove R2 storage credentials from this vault? Database upload and download will stop working." onCancel={() => setConfirming(false)} onConfirm={() => void remove()} title="Remove R2 credentials?" />}
    </section>
  );
}
