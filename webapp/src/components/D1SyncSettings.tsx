import { useEffect, useState, type FormEvent } from 'react';
import { publicErrorMessage, type D1StorageDetail } from '../api/client';
import { StaleNotice, useFormReload } from '../detailRefresh';
import { ConfirmDialog } from './ConfirmDialog';
import { ChevronLeftIcon } from './Icons';
import { LoadFailure, type SettingsScreenProps } from './SettingsShared';

interface D1Draft {
  account_id: string;
  database_id: string;
  api_token: string;
}

function d1Draft(detail: D1StorageDetail | null): D1Draft {
  return {
    account_id: detail?.account_id ?? '',
    database_id: detail?.database_id ?? '',
    api_token: '',
  };
}

export function D1SyncScreen({ client, navigate }: SettingsScreenProps) {
  const [detail, setDetail] = useState<D1StorageDetail | null | undefined>(undefined);
  const [draft, setDraft] = useState<D1Draft>(() => d1Draft(null));
  const [busy, setBusy] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const reload = useFormReload('d1');

  function applyD1(loaded: D1StorageDetail | null) {
    setDetail(loaded);
    setDraft(d1Draft(loaded));
  }

  useEffect(() => {
    let current = true;
    const background = reload.start();
    if (!background) {
      setDetail(undefined);
      setError(null);
    }
    void client.getD1Storage().then(
      (loaded) => {
        if (!current) return;
        setError(null);
        reload.loaded(loaded, applyD1);
      },
      () => {
        if (!current) return;
        setError('D1 credentials could not be loaded.');
        reload.fail(background);
      },
    );
    return () => { current = false; };
  }, [client, reload.epoch, reload.attempt]);

  function change(field: keyof D1Draft, value: string) {
    setDraft((current) => ({ ...current, [field]: value }));
  }

  const original = d1Draft(detail ?? null);
  const dirty = draft.account_id !== original.account_id
    || draft.database_id !== original.database_id
    || Boolean(draft.api_token);
  const valid = Boolean(
    draft.account_id.trim()
    && draft.database_id.trim()
    && (detail?.has_api_token || draft.api_token.trim()),
  );
  reload.markDirty(dirty);
  function resetD1() {
    if (reload.stale) {
      reload.accept(applyD1);
      return;
    }
    setDraft(d1Draft(detail ?? null));
  }

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!valid || busy || reload.blocked) return;
    setBusy(true);
    setError(null);
    try {
      const saved = await client.saveD1Storage({
        display_name: detail?.display_name ?? 'D1',
        account_id: draft.account_id.trim(),
        database_id: draft.database_id.trim(),
        api_token: draft.api_token.trim() ? draft.api_token.trim() : null,
      });
      applyD1(saved);
      reload.remember(saved);
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'D1 credentials could not be saved.'));
    } finally {
      setBusy(false);
    }
  }

  async function remove() {
    setConfirming(false);
    if (!detail || busy) return;
    setBusy(true);
    setError(null);
    try {
      await client.deleteD1Storage();
      applyD1(null);
      reload.remember(null);
      navigate({ type: 'show-settings-api-keys' });
    } catch {
      setError('D1 credentials could not be removed.');
    } finally {
      setBusy(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="D1 sync settings">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-api-keys' })} type="button"><ChevronLeftIcon /><span>API Keys</span></button>
      {error && (detail === undefined || reload.refreshFailed) && <LoadFailure message={error} retry={reload.retry} />}
      {detail !== undefined && <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
        <fieldset disabled={busy}>
          <label>Account ID<input autoComplete="off" autoFocus className="cha-form-control" onChange={(event) => change('account_id', event.target.value)} placeholder="Paste account ID" value={draft.account_id} /></label>
          <label>Database ID<input autoComplete="off" className="cha-form-control" onChange={(event) => change('database_id', event.target.value)} placeholder="Paste database ID" value={draft.database_id} /></label>
          <label>API token<input autoComplete="off" className="cha-form-control" onChange={(event) => change('api_token', event.target.value)} placeholder={detail?.has_api_token ? 'Leave blank to keep the current token' : 'Paste API token'} type="password" value={draft.api_token} /></label>
        </fieldset>
        {error && !reload.refreshFailed && <p className="cha-error-message" role="alert">{error}</p>}
        {reload.stale && <StaleNotice onReload={() => reload.accept(applyD1)} />}
        <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={(!dirty && !reload.stale) || busy} onClick={resetD1} type="button">Reset</button><button className="cha-button cha-button-primary" disabled={!valid || busy || reload.blocked} type="submit">Save D1 credentials</button></div>
        {detail && <div className="cha-settings-form-actions"><button className="cha-button cha-button-danger" disabled={busy} onClick={() => setConfirming(true)} type="button">Disconnect from D1</button></div>}
      </form>}
      {confirming && <ConfirmDialog confirmLabel="Disconnect from D1" message="Disconnect this vault from D1 on this machine? Its D1 queue and its unsent changes will be deleted." onCancel={() => setConfirming(false)} onConfirm={() => void remove()} title="Disconnect from D1?" />}
    </section>
  );
}
