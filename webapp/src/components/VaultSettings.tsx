import {
  useEffect,
  useRef,
  useState,
  type FormEvent,
} from 'react';
import {
  ChaError,
  publicErrorMessage,
  type ChaClient,
  type VaultDetail,
  type VaultUploadCheck,
} from '../api/client';
import { DetailRefreshProvider, StaleNotice, useFormReload } from '../detailRefresh';
import { validateBootstrap } from '../state/bootstrap';
import { useLoad } from '../useLoad';
import { ConfirmDialog } from './ConfirmDialog';
import { EditableTitle } from './DetailActions';
import {
  ChevronLeftIcon,
  DatabaseIcon,
  DownloadIcon,
  EyeIcon,
  EyeOffIcon,
  FileDownIcon,
  FileUpIcon,
  PlusIcon,
} from './Icons';
import { PasswordDialog } from './PasswordDialog';
import { TransliteratingInput } from './TransliterationMode';
import {
  BackToSettings,
  LoadFailure,
  SettingsRow,
  type SettingsScreenProps,
} from './SettingsShared';

const loadVaults = (client: ChaClient) => client.listVaults();

function formatBytes(bytes: number): string {
  const units = ['bytes', 'KB', 'MB', 'GB', 'TB', 'PB'];
  let size = bytes;
  let unit = 0;
  while (size >= 1000 && unit < units.length - 1) {
    size /= 1000;
    unit += 1;
  }
  return `${size.toLocaleString(undefined, { maximumFractionDigits: unit ? 1 : 0 })} ${units[unit]}`;
}

function VaultPasswordInput({
  autoFocus = false,
  id,
  onChange,
  value,
}: {
  autoFocus?: boolean;
  id: string;
  onChange(value: string): void;
  value: string;
}) {
  const [visible, setVisible] = useState(false);

  return (
    <div className="cha-password-input">
      <label htmlFor={id}>Password</label>
      <div className="cha-password-field">
        <input
          autoComplete="new-password"
          autoFocus={autoFocus}
          className="cha-form-control"
          id={id}
          onChange={(event) => onChange(event.target.value)}
          type={visible ? 'text' : 'password'}
          value={value}
        />
        <button
          aria-label={visible ? 'Hide password' : 'Show password'}
          className="cha-password-visibility"
          onClick={() => setVisible((current) => !current)}
          type="button"
        >
          {visible ? <EyeOffIcon /> : <EyeIcon />}
        </button>
      </div>
    </div>
  );
}

export function VaultsScreen({ client, navigate }: SettingsScreenProps) {
  const { data: vaults, error, retry } = useLoad(
    client, loadVaults, 'Vaults could not be loaded.',
  );

  return (
    <section className="cha-screen cha-navigation" aria-label="Vaults settings">
      <BackToSettings navigate={navigate} />
      {vaults === null && !error && <p className="cha-state-message" role="status">Loading vaults…</p>}
      {error && <LoadFailure message={error} retry={retry} />}
      {vaults && (
        <div className="cha-list">
          <SettingsRow
            description="Create an empty vault or copy an existing one"
            icon={<PlusIcon />}
            label="New vault"
            onClick={() => navigate({ type: 'show-settings-new-vault' })}
          />
          <SettingsRow
            description="Download a vault from R2"
            icon={<DownloadIcon />}
            label="Download vault"
            onClick={() => navigate({ type: 'show-settings-download-vault' })}
          />
          {vaults.map((vault) => (
            <SettingsRow
              description={vault.active ? 'Active' : undefined}
              icon={<DatabaseIcon />}
              key={vault.display_name}
              label={vault.display_name}
              onClick={() => navigate({
                type: 'inspect-vault',
                vaultName: vault.display_name,
              })}
            />
          ))}
        </div>
      )}
    </section>
  );
}

export function DownloadVaultScreen({ client, dispatch, navigate }: SettingsScreenProps) {
  const [names, setNames] = useState<string[] | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [downloaded, setDownloaded] = useState<Set<string>>(() => new Set());
  const [error, setError] = useState<string | null>(null);
  const [revision, setRevision] = useState(0);

  useEffect(() => {
    let current = true;
    setNames(null);
    setError(null);
    void client.listR2Vaults().then(
      (loaded) => { if (current) setNames(loaded); },
      (failure: unknown) => {
        if (current) {
          setError(publicErrorMessage(
            failure, 'Vaults could not be loaded from R2.',
          ));
        }
      },
    );
    return () => { current = false; };
  }, [client, revision]);

  async function download(name: string) {
    if (busy !== null) return;
    setBusy(name);
    setError(null);
    try {
      const vault = await client.downloadR2Vault(name);
      setDownloaded((current) => new Set(current).add(name));
      dispatch({ type: 'vault-downloaded', vault });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The vault could not be downloaded.'));
    } finally {
      setBusy(null);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="Download vault settings">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-vaults' })} type="button"><ChevronLeftIcon /><span>Vaults</span></button>
      {names === null && !error && <p className="cha-state-message" role="status">Loading vaults from R2…</p>}
      {names === null && error && <LoadFailure message={error} retry={() => setRevision((value) => value + 1)} />}
      {names && names.length === 0 && <p className="cha-empty-list">No vaults found in R2</p>}
      {names && names.length > 0 && (
        <div className="cha-list">
          {names.map((name) => (
            <SettingsRow
              description={busy === name ? 'Downloading…' : downloaded.has(name) ? 'Downloaded' : undefined}
              disabled={busy !== null || downloaded.has(name)}
              icon={<DatabaseIcon />}
              key={name}
              label={name}
              onClick={() => void download(name)}
              trailingIcon={<DownloadIcon className="cha-chevron" />}
            />
          ))}
        </div>
      )}
      {names !== null && error && <p className="cha-error-message" role="alert">{error}</p>}
    </section>
  );
}

export function MergeVaultScreen({ client, dispatch, navigate, state }: SettingsScreenProps) {
  const { data: vaults, error: loadError, retry } = useLoad(
    client, loadVaults, 'Vaults could not be loaded.',
  );
  const [source, setSource] = useState('');
  const [confirming, setConfirming] = useState(false);
  const [passwordPrompt, setPasswordPrompt] = useState(false);
  const [passwordError, setPasswordError] = useState<string | null>(null);
  const [pending, setPending] = useState(false);
  const [complete, setComplete] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const pendingRef = useRef(false);

  const destination = vaults?.find((vault) => vault.active)?.display_name ?? '';
  const sources = vaults?.filter((vault) => !vault.active) ?? [];

  async function merge(password?: string) {
    if (!source || !destination || pendingRef.current) return;
    pendingRef.current = true;
    setPending(true);
    setError(null);
    setComplete(false);
    if (password) setPasswordError(null);
    try {
      await client.mergeVault(source, password);
      setPasswordPrompt(false);
      setPasswordError(null);
      try {
        dispatch({
          type: 'bootstrap-refreshed',
          bootstrap: validateBootstrap(await client.getBootstrap()),
        });
      } catch {
        // Discovery refresh is non-critical; merge already succeeded.
      }
      setComplete(true);
    } catch (failure: unknown) {
      if (failure instanceof ChaError && failure.code === 'source_vault_password_required') {
        setPasswordPrompt(true);
        setPasswordError(password
          ? publicErrorMessage(failure, 'The vault could not be merged.')
          : null);
      } else {
        setPasswordPrompt(false);
        setPasswordError(null);
        setError(publicErrorMessage(failure, 'The vault could not be merged.'));
      }
    } finally {
      pendingRef.current = false;
      setPending(false);
    }
  }

  const ready = vaults !== null;

  return (
    <section className="cha-screen cha-navigation" aria-label="Merge vault settings">
      <button
        className="cha-back-row"
        onClick={() => navigate(state.inspectedVaultName
          ? { type: 'inspect-vault', vaultName: state.inspectedVaultName }
          : { type: 'show-settings-vaults' })}
        type="button"
      ><ChevronLeftIcon /><span>{state.inspectedVaultName ?? 'Vaults'}</span></button>
      {!ready && !loadError && <p className="cha-state-message" role="status">Loading vaults…</p>}
      {loadError && !ready && <LoadFailure message={loadError} retry={retry} />}
      {ready && sources.length === 0 && <p className="cha-empty-list">No other vaults</p>}
      {ready && sources.length > 0 && (
        <form
          className="cha-settings-form"
          onSubmit={(event) => {
            event.preventDefault();
            if (!source || !destination || pending) return;
            setConfirming(true);
          }}
        >
          <fieldset disabled={pending}>
            <label>Source vault<select className="cha-form-control" onChange={(event) => setSource(event.target.value)} value={source}><option value="">Select a vault</option>{sources.map((vault) => <option key={vault.display_name} value={vault.display_name}>{vault.display_name}</option>)}</select></label>
          </fieldset>
          {complete && <p className="cha-state-message" role="status">Merge complete</p>}
          {error && <p className="cha-error-message" role="alert">{error}</p>}
          <div className="cha-settings-form-actions"><button className="cha-button cha-button-primary" disabled={!source || !destination || pending} type="submit">{pending ? 'Merging…' : 'Merge'}</button></div>
        </form>
      )}
      {confirming && (
        <ConfirmDialog
          confirmLabel="Merge"
          message={`Merge “${source}” into “${destination}”? Source files overwrite destination files at matching paths. This cannot be undone.`}
          onCancel={() => setConfirming(false)}
          onConfirm={() => {
            setConfirming(false);
            void merge();
          }}
          title="Merge vault?"
        />
      )}
      {passwordPrompt && (
        <PasswordDialog
          error={passwordError}
          name={source}
          onCancel={() => {
            setPasswordPrompt(false);
            setPasswordError(null);
          }}
          onSubmit={(password) => void merge(password)}
        />
      )}
    </section>
  );
}

export function NewVaultScreen({ client, navigate }: SettingsScreenProps) {
  const [vaults, setVaults] = useState<VaultDetail[] | null>(null);
  const [name, setName] = useState('');
  const [copyFrom, setCopyFrom] = useState('');
  const [protectedVault, setProtectedVault] = useState(false);
  const [password, setPassword] = useState('');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let current = true;
    void client.listVaults().then(
      (loaded) => {
        if (!current) return;
        setVaults(loaded);
      },
      (failure: unknown) => {
        if (current) setError(publicErrorMessage(failure, 'Vaults could not be loaded.'));
      },
    );
    return () => { current = false; };
  }, [client]);

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!name.trim() || saving || (protectedVault && !password)) return;
    setSaving(true);
    setError(null);
    try {
      const created = await client.createVault({
        display_name: name.trim(),
        copy_from: copyFrom || null,
        password: protectedVault ? password : null,
      });
      navigate({ type: 'vault-created', vault: created });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The vault could not be created.'));
      setSaving(false);
    }
  }

  const ready = vaults !== null && vaults.length > 0;
  return (
    <section className="cha-screen cha-navigation" aria-label="New vault settings">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-vaults' })} type="button"><ChevronLeftIcon /><span>Vaults</span></button>
      {!ready && !error && <p className="cha-state-message" role="status">Loading vaults…</p>}
      {ready && (
        <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
          <fieldset disabled={saving}>
            <TransliteratingInput autoFocus className="cha-form-control" id="cha-new-vault-name" label="Display name" onValueChange={setName} placeholder="e.g. Projects" value={name} />
            <label>Initial database<select className="cha-form-control" onChange={(event) => setCopyFrom(event.target.value)} value={copyFrom}><option value="">New empty vault</option>{vaults.map((vault) => <option key={vault.display_name} value={vault.display_name}>Copy {vault.display_name}</option>)}</select></label>
            <label className="cha-checkbox-row"><input checked={protectedVault} onChange={(event) => { setProtectedVault(event.target.checked); if (!event.target.checked) setPassword(''); setError(null); }} type="checkbox" />Protected vault</label>
            {protectedVault && <VaultPasswordInput id="cha-new-vault-password" onChange={(value) => { setPassword(value); setError(null); }} value={password} />}
          </fieldset>
          {error && <p className="cha-error-message" role="alert">{error}</p>}
          <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving} onClick={() => navigate({ type: 'show-settings-vaults' })} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!name.trim() || saving || (protectedVault && !password)} type="submit">{saving ? 'Creating…' : 'Create vault'}</button></div>
        </form>
      )}
      {error && !ready && <p className="cha-error-message" role="alert">{error}</p>}
    </section>
  );
}

export function VaultScreen({ client, dispatch, navigate, state }: SettingsScreenProps) {
  const selectedName = state.inspectedVaultName;
  const [detail, setDetail] = useState<VaultDetail | null>(null);
  const [vaultCount, setVaultCount] = useState(0);
  const [enableProtection, setEnableProtection] = useState(false);
  const [password, setPassword] = useState('');
  const [saving, setSaving] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const [confirmingOperation, setConfirmingOperation] = useState<'upload' | 'download' | 'import' | null>(null);
  const [uploadCheck, setUploadCheck] = useState<VaultUploadCheck | null>(null);
  const remoteVaultMissing = uploadCheck?.status === 'mismatch' && uploadCheck.etag === null;
  const [checkingUpload, setCheckingUpload] = useState(false);
  const [pendingOperation, setPendingOperation] = useState<'upload' | 'download' | 'import' | 'export' | null>(null);
  const [operationComplete, setOperationComplete] = useState<string | null>(null);
  const [operationError, setOperationError] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const reload = useFormReload(selectedName);
  const busy = pendingOperation !== null || checkingUpload || saving || deleting;
  const canTransferR2 = state.bootstrap?.capabilities?.can_transfer_r2 === true;
  const canModify = state.bootstrap?.capabilities?.can_modify === true;

  function applyVault(value: { found: VaultDetail; count: number }) {
    setVaultCount(value.count);
    setDetail(value.found);
    setEnableProtection(false);
    setPassword('');
  }

  useEffect(() => {
    let current = true;
    const background = reload.start();
    if (!background) {
      setDetail(null);
      setError(null);
    }
    if (!selectedName) return () => { current = false; };
    void client.listVaults().then(
      (loaded) => {
        if (!current) return;
        const found = loaded.find(({ display_name: currentName }) => currentName === selectedName);
        if (!found) {
          setError('That vault was not found.');
          reload.fail(background);
          return;
        }
        setError(null);
        reload.loaded({ found, count: loaded.length }, applyVault);
      },
      (failure: unknown) => {
        if (!current) return;
        setError(publicErrorMessage(failure, 'Vault settings could not be loaded.'));
        reload.fail(background);
      },
    );
    return () => { current = false; };
  }, [client, reload.epoch, reload.attempt, selectedName]);

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!detail || !selectedName || !enableProtection || !password
        || saving || deleting) return;
    setSaving(true);
    setError(null);
    try {
      const updated = await client.updateVault(
        selectedName,
        { display_name: detail.display_name, password },
      );
      setDetail(updated);
      setEnableProtection(false);
      setPassword('');
      reload.remember({ found: updated, count: vaultCount });
      dispatch({ type: 'vault-updated', previousName: selectedName, vault: updated });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Vault settings could not be saved.'));
    } finally {
      setSaving(false);
    }
  }

  async function remove() {
    setConfirming(false);
    if (!detail || !selectedName || saving || deleting || !detail.can_delete) return;
    setDeleting(true);
    setError(null);
    try {
      await client.deleteVault(selectedName);
      navigate({ type: 'vault-deleted', vaultName: selectedName });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The vault could not be deleted.'));
      setDeleting(false);
    }
  }

  async function runOperation(
    operation: 'upload' | 'download' | 'import' | 'export',
    check: VaultUploadCheck | null = null,
  ) {
    setConfirmingOperation(null);
    if (!detail?.active || busy) return;
    if (operation === 'upload' && !check) return;
    setPendingOperation(operation);
    setOperationComplete(null);
    setOperationError(null);
    try {
      const actions = {
        upload: { run: () => client.uploadVault(check!), past: 'Uploaded', bytes: true },
        download: { run: () => client.downloadVault(), past: 'Downloaded', bytes: true },
        import: { run: () => client.importVault(), past: 'Imported', bytes: false },
        export: { run: () => client.exportVault(), past: 'Exported', bytes: false },
      };
      const { run, past, bytes } = actions[operation];
      const count = await run();
      setOperationComplete(`${past} ${bytes ? formatBytes(count) : `${count.toLocaleString()} files`}.`);
    } catch (failure: unknown) {
      const label = operation[0].toUpperCase() + operation.slice(1);
      setOperationError(publicErrorMessage(failure, `${label} failed.`));
    } finally {
      setPendingOperation(null);
    }
  }

  async function requestUpload() {
    if (!detail?.active || busy) return;
    setCheckingUpload(true);
    setOperationComplete(null);
    setOperationError(null);
    try {
      const check = await client.checkVaultUpload();
      setUploadCheck(check);
      if (check.status === 'match') {
        setCheckingUpload(false);
        await runOperation('upload', check);
        return;
      }
      setConfirmingOperation('upload');
    } catch (failure: unknown) {
      setOperationError(publicErrorMessage(failure, 'Upload check failed.'));
    } finally {
      setCheckingUpload(false);
    }
  }

  const protectionDirty = enableProtection || password !== '';
  reload.markDirty(protectionDirty);

  function reset() {
    setError(null);
    if (reload.stale) {
      reload.accept(applyVault);
      return;
    }
    setEnableProtection(false);
    setPassword('');
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="Vault settings">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-vaults' })} type="button"><ChevronLeftIcon /><span>Vaults</span></button>
      {(detail || selectedName) && <div className="cha-detail-actions">
        {detail ? <DetailRefreshProvider value={reload.context}><EditableTitle
          available disabled={saving || deleting}
          id={selectedName} name={detail.display_name} subject="Vault"
          onSave={async (display_name) => {
            setSaving(true);
            try {
              const saved = await client.updateVault(selectedName!, { display_name, password: null });
              setDetail(saved);
              reload.remember({ found: saved, count: vaultCount });
              dispatch({ type: 'vault-updated', previousName: selectedName!, vault: saved });
            } finally {
              setSaving(false);
            }
          }}
        /></DetailRefreshProvider> : <h1>{selectedName}</h1>}
      </div>}
      {!selectedName && <p className="cha-state-message">No vault is selected.</p>}
      {selectedName && !detail && !error && <p className="cha-state-message" role="status">Loading vault…</p>}
      {error && (!detail || reload.refreshFailed) && <LoadFailure message={error} retry={reload.retry} />}
      {detail && (
        <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
          <fieldset disabled={saving || deleting}>
            <label className="cha-checkbox-row"><input checked={detail.protected || enableProtection} disabled={detail.protected} onChange={(event) => { setEnableProtection(event.target.checked); if (!event.target.checked) setPassword(''); setError(null); }} type="checkbox" />Protected vault</label>
            {!detail.protected && enableProtection && <VaultPasswordInput autoFocus id="cha-vault-password" onChange={(value) => { setPassword(value); setError(null); }} value={password} />}
          </fieldset>
          {detail.active && <p className="cha-settings-note">This vault is active.</p>}
          {detail.active && (
            <div className="cha-list">
              <SettingsRow
                disabled={busy}
                icon={<DatabaseIcon />}
                label="Merge"
                onClick={() => navigate({ type: 'show-settings-merge-vault' })}
              />
              <SettingsRow
                description={!canTransferR2 ? 'R2 storage is not configured' : undefined}
                disabled={!canTransferR2 || busy}
                icon={<FileUpIcon />}
                label={checkingUpload ? 'Checking…'
                  : pendingOperation === 'upload' ? 'Uploading…' : 'Upload'}
                onClick={() => void requestUpload()}
                trailingIcon={null}
              />
              <SettingsRow
                description={!canTransferR2 ? 'R2 storage is not configured' : undefined}
                disabled={!canTransferR2 || busy}
                icon={<DownloadIcon />}
                label={pendingOperation === 'download' ? 'Downloading…' : 'Download'}
                onClick={() => setConfirmingOperation('download')}
                trailingIcon={null}
              />
              <SettingsRow
                description={!canModify ? 'Modify directory is not configured' : undefined}
                disabled={!canModify || busy}
                icon={<FileDownIcon />}
                label={pendingOperation === 'import' ? 'Importing…' : 'Import'}
                onClick={() => setConfirmingOperation('import')}
                trailingIcon={null}
              />
              <SettingsRow
                description={!canModify ? 'Modify directory is not configured' : undefined}
                disabled={!canModify || busy}
                icon={<FileUpIcon />}
                label={pendingOperation === 'export' ? 'Exporting…' : 'Export'}
                onClick={() => void runOperation('export')}
                trailingIcon={null}
              />
            </div>
          )}
          {operationComplete && <p className="cha-state-message" role="status">{operationComplete}</p>}
          {operationError && <p className="cha-error-message" role="alert">{operationError}</p>}
          {!detail.can_delete && <p className="cha-settings-note">{vaultCount === 1 ? 'The last vault cannot be deleted.' : 'Switch to another vault before deleting this one.'}</p>}
          {error && !reload.refreshFailed && <p className="cha-error-message" role="alert">{error}</p>}
          {reload.stale && <StaleNotice onReload={() => reload.accept(applyVault)} />}
          {!detail.protected && enableProtection && <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving || deleting} onClick={reset} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!password || saving || deleting || reload.blocked} type="submit">{saving ? 'Protecting…' : 'Protect vault'}</button></div>}
          <div className="cha-settings-form-actions"><button className="cha-button cha-button-danger" disabled={!detail.can_delete || saving || deleting} onClick={() => setConfirming(true)} type="button">{deleting ? 'Deleting…' : 'Delete vault'}</button></div>
        </form>
      )}
      {confirming && (
        <ConfirmDialog
          confirmLabel="Delete vault"
          message={`Remove “${detail?.display_name ?? 'this vault'}” from CHA? Its database and folders will be kept.`}
          onCancel={() => setConfirming(false)}
          onConfirm={() => void remove()}
          title="Delete vault?"
        />
      )}
      {confirmingOperation === 'upload' && (
        <ConfirmDialog
          confirmLabel="Upload"
          message={remoteVaultMissing
            ? `The R2 copy of “${detail?.display_name ?? 'this vault'}” no longer exists. Upload it again?`
            : uploadCheck?.status === 'mismatch'
            ? `The R2 version of “${detail?.display_name ?? 'this vault'}” does not match the version recorded locally. Upload and overwrite the R2 vault?`
            : `No R2 version is recorded for “${detail?.display_name ?? 'this vault'}”. Upload and overwrite any existing R2 vault?`}
          onCancel={() => setConfirmingOperation(null)}
          onConfirm={() => void runOperation('upload', uploadCheck)}
          title={remoteVaultMissing ? 'Upload the vault again?' : 'Overwrite the R2 vault?'}
        />
      )}
      {confirmingOperation === 'download' && (
        <ConfirmDialog
          confirmLabel="Download"
          initialFocus="cancel"
          message={`Download “${detail?.display_name ?? 'this vault'}” from R2 and replace its local database, including all local notes and folders? The previous database will replace the existing .bac backup.`}
          onCancel={() => setConfirmingOperation(null)}
          onConfirm={() => void runOperation('download')}
          title="Replace the local database?"
        />
      )}
      {confirmingOperation === 'import' && (
        <ConfirmDialog
          confirmLabel="Import"
          message="Replace this vault’s workspace configuration with the contents of its modify directory?"
          onCancel={() => setConfirmingOperation(null)}
          onConfirm={() => void runOperation('import')}
          title="Import workspace configuration?"
        />
      )}
    </section>
  );
}
