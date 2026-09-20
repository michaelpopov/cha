import {
  useEffect,
  useRef,
  useState,
  type Dispatch,
  type FormEvent,
  type ReactNode,
} from 'react';

import {
  ChaError,
  publicErrorMessage,
  type ApiKeyDetail,
  type ChaClient,
  type ProviderDetail,
  type ProviderSummary,
  type ProviderUpdate,
  type R2StorageDetail,
  type StyleDetail,
  type StyleUpdate,
  type VoiceDetail,
  type VoiceInputSettings,
  type VoiceOutputSettings,
  type VoiceUpdate,
  type VaultDetail,
} from '../api/client';
import {
  nativeSpeechFromClient,
  TextToSpeechError,
  TextToSpeechSession,
  speechVoice,
  useTextToSpeechConfiguration,
} from '../textToSpeech';
import { validateBootstrap } from '../state/bootstrap';
import { reloadForVoiceSettings } from '../state/voiceSettingsReload';
import type { AppAction, AppState } from '../state/view';
import { useLoad } from '../useLoad';
import { voiceClasses } from './characterAppearance';
import { ConfirmDialog } from './ConfirmDialog';
import { PasswordDialog } from './PasswordDialog';
import {
  CharacterIcon,
  ChevronLeftIcon,
  ChevronRightIcon,
  DatabaseIcon,
  DownloadIcon,
  EyeIcon,
  EyeOffIcon,
  KeyIcon,
  PlusIcon,
  SpeakerIcon,
  SettingsIcon,
  StopIcon,
} from './Icons';
import { TransliteratingInput } from './TransliterationMode';

interface SettingsScreenProps {
  client: ChaClient;
  dispatch: Dispatch<AppAction>;
  sessionReport: ReactNode;
  state: AppState;
}

const loadVaults = (client: ChaClient) => client.listVaults();
const loadProviders = (client: ChaClient) => client.listProviders();
const loadStyles = (client: ChaClient) => client.listStyles();
const loadVoices = (client: ChaClient) => client.listVoices();
const loadApiKeys = (client: ChaClient) => client.listApiKeys();

function BackToSettings({ dispatch }: { dispatch: Dispatch<AppAction> }) {
  return (
    <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings' })} type="button">
      <ChevronLeftIcon />
      <span>Settings</span>
    </button>
  );
}

function SettingsRow({
  description,
  disabled = false,
  icon,
  label,
  onClick,
  trailingIcon,
}: {
  description?: string;
  disabled?: boolean;
  icon: ReactNode;
  label: string;
  onClick(): void;
  trailingIcon?: ReactNode;
}) {
  return (
    <button className="cha-list-action cha-settings-row" disabled={disabled} onClick={onClick} type="button">
      <span className="cha-list-icon">{icon}</span>
      <span className="cha-list-copy">
        <span className="cha-primary-line">{label}</span>
        {description && <span className="cha-secondary-line">{description}</span>}
      </span>
      {trailingIcon ?? <ChevronRightIcon className="cha-chevron" />}
    </button>
  );
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

export function SettingsNavigation({ dispatch }: { dispatch: Dispatch<AppAction> }) {
  return (
    <section className="cha-settings-card" aria-labelledby="cha-configuration-settings-title">
      <header className="cha-settings-card-header">
        <h2 id="cha-configuration-settings-title">Configuration</h2>
        <p>Configure vaults, inference, character appearance, and voice output.</p>
      </header>
      <div className="cha-settings-links">
        <SettingsRow
          description="Databases, mirrors, and editable workspace paths"
          icon={<DatabaseIcon />}
          label="Vaults"
          onClick={() => dispatch({ type: 'show-settings-vaults' })}
        />
        <SettingsRow
          description="Endpoints, models, defaults, and authentication"
          icon={<SettingsIcon />}
          label="Providers"
          onClick={() => dispatch({ type: 'show-settings-providers' })}
        />
        <SettingsRow
          description="Typography and color presets for characters"
          icon={<CharacterIcon />}
          label="Styles"
          onClick={() => dispatch({ type: 'show-settings-styles' })}
        />
        <SettingsRow
          description="Voices used by characters for spoken responses"
          icon={<SpeakerIcon />}
          label="Voices"
          onClick={() => dispatch({ type: 'show-settings-voices' })}
        />
        <SettingsRow
          description="Model and R2 credentials stored in each vault"
          icon={<KeyIcon />}
          label="API Keys"
          onClick={() => dispatch({ type: 'show-settings-api-keys' })}
        />
      </div>
    </section>
  );
}

export function VaultsScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const { data: vaults, error, retry } = useLoad(
    client, loadVaults, 'Vaults could not be loaded.',
  );

  return (
    <section className="cha-screen cha-navigation" aria-label="Vaults settings">
      <BackToSettings dispatch={dispatch} />
      {sessionReport}
      {vaults === null && !error && <p className="cha-state-message" role="status">Loading vaults…</p>}
      {error && <LoadFailure message={error} retry={retry} />}
      {vaults && (
        <div className="cha-list">
          <SettingsRow
            description="Create an empty vault or copy an existing one"
            icon={<PlusIcon />}
            label="New vault"
            onClick={() => dispatch({ type: 'show-settings-new-vault' })}
          />
          <SettingsRow
            description="Download a vault from R2"
            icon={<DownloadIcon />}
            label="Download vault"
            onClick={() => dispatch({ type: 'show-settings-download-vault' })}
          />
          <SettingsRow
            icon={<DatabaseIcon />}
            label="Merge into active vault"
            onClick={() => dispatch({ type: 'show-settings-merge-vault' })}
          />
          {vaults.map((vault) => (
            <SettingsRow
              description={vault.active ? 'Active' : undefined}
              icon={<DatabaseIcon />}
              key={vault.display_name}
              label={vault.display_name}
              onClick={() => dispatch({
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

export function DownloadVaultScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
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
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-vaults' })} type="button"><ChevronLeftIcon /><span>Vaults</span></button>
      {sessionReport}
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

// Transcription connects directly from the browser, so changing its origin
// requires reloading the page with the updated content security policy.
async function voiceInputOrigin(client: ChaClient): Promise<string> {
  const input = await client.getVoiceInputSettings();
  return input ? new URL(input.url).origin : '';
}

export function MergeVaultScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const { data: vaults, error: loadError, retry } = useLoad(
    client, loadVaults, 'Vaults could not be loaded.',
  );
  const [source, setSource] = useState('');
  const [confirming, setConfirming] = useState(false);
  const [passwordPrompt, setPasswordPrompt] = useState(false);
  const [passwordError, setPasswordError] = useState<string | null>(null);
  const [pending, setPending] = useState(false);
  const [complete, setComplete] = useState(false);
  const [reloadNeeded, setReloadNeeded] = useState(false);
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
      const voiceBefore = await voiceInputOrigin(client).catch(() => null);
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
      // When the transcription origin is unknown, offering a reload is harmless.
      const voiceChanged = await voiceInputOrigin(client).then(
        (voiceAfter) => voiceBefore === null || voiceAfter !== voiceBefore,
        () => true,
      );
      setReloadNeeded((needed) => needed || voiceChanged);
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
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-vaults' })} type="button"><ChevronLeftIcon /><span>Vaults</span></button>
      {sessionReport}
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
          {complete && reloadNeeded && (
            <div className="cha-state-message">
              <p>Reload CHA to use the merged voice settings.</p>
              <button className="cha-button cha-button-ghost" onClick={() => window.location.reload()} type="button">Reload</button>
            </div>
          )}
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

export function NewVaultScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
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
      dispatch({ type: 'vault-created', vault: created });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The vault could not be created.'));
      setSaving(false);
    }
  }

  const ready = vaults !== null && vaults.length > 0;
  return (
    <section className="cha-screen cha-navigation" aria-label="New vault settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-vaults' })} type="button"><ChevronLeftIcon /><span>Vaults</span></button>
      {sessionReport}
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
          <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving} onClick={() => dispatch({ type: 'show-settings-vaults' })} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!name.trim() || saving || (protectedVault && !password)} type="submit">{saving ? 'Creating…' : 'Create vault'}</button></div>
        </form>
      )}
      {error && !ready && <p className="cha-error-message" role="alert">{error}</p>}
    </section>
  );
}

export function VaultScreen({ client, dispatch, sessionReport, state }: SettingsScreenProps) {
  const selectedName = state.inspectedVaultName;
  const [detail, setDetail] = useState<VaultDetail | null>(null);
  const [vaultCount, setVaultCount] = useState(0);
  const [enableProtection, setEnableProtection] = useState(false);
  const [password, setPassword] = useState('');
  const [saving, setSaving] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [revision, setRevision] = useState(0);

  useEffect(() => {
    let current = true;
    setDetail(null);
    setError(null);
    if (!selectedName) return () => { current = false; };
    void client.listVaults().then(
      (loaded) => {
        if (!current) return;
        setVaultCount(loaded.length);
        const found = loaded.find(({ display_name: currentName }) => currentName === selectedName);
        if (!found) {
          setError('That vault was not found.');
          return;
        }
        setDetail(found);
        setEnableProtection(false);
        setPassword('');
      },
      (failure: unknown) => {
        if (current) setError(publicErrorMessage(failure, 'Vault settings could not be loaded.'));
      },
    );
    return () => { current = false; };
  }, [client, revision, selectedName]);

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
      dispatch({ type: 'vault-deleted', vaultName: selectedName });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The vault could not be deleted.'));
      setDeleting(false);
    }
  }

  function reset() {
    setEnableProtection(false);
    setPassword('');
    setError(null);
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="Vault settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-vaults' })} type="button"><ChevronLeftIcon /><span>Vaults</span></button>
      {sessionReport}
      {!selectedName && <p className="cha-state-message">No vault is selected.</p>}
      {selectedName && !detail && !error && <p className="cha-state-message" role="status">Loading vault…</p>}
      {error && !detail && <LoadFailure message={error} retry={() => setRevision((value) => value + 1)} />}
      {detail && (
        <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
          <fieldset disabled={saving || deleting}>
            <label className="cha-checkbox-row"><input checked={detail.protected || enableProtection} disabled={detail.protected} onChange={(event) => { setEnableProtection(event.target.checked); if (!event.target.checked) setPassword(''); setError(null); }} type="checkbox" />Protected vault</label>
            {!detail.protected && enableProtection && <VaultPasswordInput autoFocus id="cha-vault-password" onChange={(value) => { setPassword(value); setError(null); }} value={password} />}
          </fieldset>
          {detail.active && <p className="cha-settings-note">This vault is active.</p>}
          {!detail.can_delete && <p className="cha-settings-note">{vaultCount === 1 ? 'The last vault cannot be deleted.' : 'Switch to another vault before deleting this one.'}</p>}
          {error && <p className="cha-error-message" role="alert">{error}</p>}
          {!detail.protected && enableProtection && <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving || deleting} onClick={reset} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!password || saving || deleting} type="submit">{saving ? 'Protecting…' : 'Protect vault'}</button></div>}
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
    </section>
  );
}

function LoadFailure({ message, retry }: { message: string; retry(): void }) {
  return (
    <div className="cha-state-message cha-error-message" role="alert">
      <p>{message}</p>
      <button className="cha-button cha-button-ghost" onClick={retry} type="button">Try again</button>
    </div>
  );
}

function UsedBy({ empty, items }: { empty: string; items: string[] }) {
  return (
    <section className="cha-settings-usage">
      <h2>Used by</h2>
      {items.length
        ? <ul>{items.map((item) => <li key={item}>{item}</li>)}</ul>
        : <p>{empty}</p>}
    </section>
  );
}

export function ProvidersScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const { data: providers, error, retry } = useLoad(
    client, loadProviders, 'Providers could not be loaded.',
  );

  return (
    <section className="cha-screen cha-navigation" aria-label="Providers settings">
      <BackToSettings dispatch={dispatch} />
      {sessionReport}
      {providers === null && !error && <p className="cha-state-message" role="status">Loading providers…</p>}
      {error && <LoadFailure message={error} retry={retry} />}
      {providers && (
        <div className="cha-list">
          <SettingsRow
            description="Start from defaults or copy existing settings"
            icon={<PlusIcon />}
            label="New provider"
            onClick={() => dispatch({ type: 'show-settings-new-provider' })}
          />
          {providers.length === 0 && <p className="cha-empty-list">No providers configured</p>}
          {providers.map((provider) => (
            <SettingsRow
              description={`${provider.model} · ${provider.host}`}
              icon={<SettingsIcon />}
              key={provider.id}
              label={provider.display_name}
              onClick={() => dispatch({
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

export function NewProviderScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
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
      dispatch({
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
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-providers' })} type="button"><ChevronLeftIcon /><span>Providers</span></button>
      {sessionReport}
      {providers === null && !error && <p className="cha-state-message" role="status">Loading providers…</p>}
      {providers !== null && (
        <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
          <fieldset disabled={saving}>
            <legend>Provider details</legend>
            <TransliteratingInput autoFocus className="cha-form-control" id="cha-new-provider-name" label="Name" onValueChange={setName} placeholder="e.g. OpenRouter" value={name} />
            <label>Initial settings<select className="cha-form-control" onChange={(event) => setCopyFrom(event.target.value)} value={copyFrom}><option value="">Default provider settings</option>{providers.map((provider) => <option key={provider.id} value={provider.id}>Copy {provider.display_name}</option>)}</select></label>
          </fieldset>
          <p className="cha-settings-note">{copyFrom ? 'The selected provider settings, including its saved API-key selection, are copied.' : 'After creation, you can configure the endpoint, model, and API key.'}</p>
          {error && <p className="cha-error-message" role="alert">{error}</p>}
          <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving} onClick={() => dispatch({ type: 'show-settings-providers' })} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!name.trim() || saving} type="submit">{saving ? 'Creating…' : 'Create provider'}</button></div>
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

interface ProviderScreenProps extends SettingsScreenProps {
  reloadVersion?: number;
}

export function ProviderScreen({
  client,
  dispatch,
  reloadVersion = 0,
  sessionReport,
  state,
}: ProviderScreenProps) {
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
  const [revision, setRevision] = useState(0);

  useEffect(() => {
    let current = true;
    setDetail(null);
    setDraft(null);
    setOpenRouterTargets('');
    setError(null);
    setTestSucceeded(false);
    if (!id) return () => { current = false; };
    void Promise.all([client.getProvider(id), client.listApiKeys()]).then(
      ([provider, loadedKeys]) => {
        if (!current) return;
        setDetail(provider);
        setDraft(providerDraft(provider, loadedKeys));
        setBaseUrl(providerBaseUrl(provider));
        setOpenRouterTargets(openRouterTargetsText(provider.openrouter_targets));
        setKeys(loadedKeys);
        dispatch({
          type: 'provider-detail-loaded',
          providerId: provider.id,
          providerName: provider.display_name,
          writable: provider.writable,
        });
      },
      (failure: unknown) => {
        if (current) setError(publicErrorMessage(failure, 'Provider settings could not be loaded.'));
      },
    );
    return () => { current = false; };
  }, [client, dispatch, id, reloadVersion, revision]);

  function change<Key extends keyof ProviderUpdate>(key: Key, value: ProviderUpdate[Key]) {
    setDraft((current) => current ? { ...current, [key]: value } : current);
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
    const targets = isOpenRouterHost(connection.host)
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
      reasoning_effort: '',
      web_search: 'off',
      temperature: usesOpenAiOAuth ? null : draft.temperature,
      max_tokens: usesOpenAiOAuth ? null : draft.max_tokens,
      cache_retention: usesOpenAiOAuth ? 'off' : draft.cache_retention,
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
      dispatch({ type: 'show-settings-providers' });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The provider could not be deleted.'));
      setDeleting(false);
    }
  }

  const dirty = detail !== null && draft !== null && (
    draft.model !== detail.model
    || draft.api !== detail.api
    || draft.auth !== detail.auth
    || draft.api_key !== (keys.some(({ id: keyId }) => keyId === detail.api_key)
      ? detail.api_key : null)
    || baseUrl !== providerBaseUrl(detail)
    || openRouterTargets !== openRouterTargetsText(detail.openrouter_targets)
  );

  function reset() {
    if (!detail) return;
    setDraft(providerDraft(detail, keys));
    setBaseUrl(providerBaseUrl(detail));
    setOpenRouterTargets(openRouterTargetsText(detail.openrouter_targets));
    setError(null);
    setTestSucceeded(false);
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="Provider settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-providers' })} type="button">
        <ChevronLeftIcon /><span>Providers</span>
      </button>
      {sessionReport}
      {!id && <p className="cha-state-message">No provider is selected.</p>}
      {id && !detail && !error && <p className="cha-state-message" role="status">Loading provider…</p>}
      {error && !detail && <LoadFailure message={error} retry={() => setRevision((value) => value + 1)} />}
      {detail && draft && (
        <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
          <fieldset aria-label="Provider details" disabled={saving || testing || deleting || !detail.writable}>
            <label>Model<input className="cha-form-control" onChange={(event) => change('model', event.target.value)} value={draft.model} /></label>
            <label>Base URL<input className="cha-form-control" onChange={(event) => { setBaseUrl(event.target.value); setError(null); setTestSucceeded(false); }} placeholder="https://api.openai.com" type="url" value={baseUrl} /></label>
            <div className="cha-settings-form-grid">
              <label>API format<select className="cha-form-control" onChange={(event) => change('api', event.target.value as ProviderUpdate['api'])} value={draft.api}><option value="responses">Responses</option><option value="chat_completions">Chat completions</option></select></label>
              <label>Credentials<select className="cha-form-control" onChange={(event) => changeCredential(event.target.value)} value={draft.auth === 'openai_subscription'
                ? openAiOAuthCredential : draft.api_key ?? ''}><option value="">No credentials</option><option value={openAiOAuthCredential}>OpenAI OAuth</option>{keys.map((key) => <option key={key.id} value={key.id}>{key.display_name}</option>)}</select></label>
            </div>
            {isOpenRouterBaseUrl(baseUrl) && <label>Inference targets<textarea className="cha-form-control cha-provider-targets" onChange={(event) => { setOpenRouterTargets(event.target.value); setError(null); setTestSucceeded(false); }} rows={3} value={openRouterTargets} /></label>}
          </fieldset>
          {!detail.writable && <p>This provider is read-only.</p>}
          <UsedBy empty="No characters use this provider." items={detail.used_by} />
          {error && <p className="cha-error-message" role="alert">{error}</p>}
          {testSucceeded && <p className="cha-settings-saved" role="status"><span aria-hidden="true" className="cha-settings-status-marker" /> Provider responded successfully.</p>}
          <div className="cha-settings-form-actions">
            <button className="cha-button" disabled={saving || testing || deleting} onClick={() => void runTest()} type="button">{testing ? 'Testing…' : 'Test'}</button>
            <button className="cha-button cha-button-ghost" disabled={!dirty || saving || testing || deleting} onClick={reset} type="button">Cancel</button>
            <button className="cha-button cha-button-primary cha-provider-save-action" disabled={!dirty || saving || testing || deleting || !detail.writable} type="submit">{saving ? 'Saving…' : 'Save changes'}</button>
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

export function StylesScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const { data: styles, error, retry } = useLoad(
    client, loadStyles, 'Styles could not be loaded.',
  );
  return (
    <section className="cha-screen cha-navigation" aria-label="Styles settings">
      <BackToSettings dispatch={dispatch} />
      {sessionReport}
      {styles === null && !error && <p className="cha-state-message" role="status">Loading styles…</p>}
      {error && <LoadFailure message={error} retry={retry} />}
      {styles && <div className="cha-list"><SettingsRow description="Start with a neutral character style" icon={<PlusIcon />} label="New style" onClick={() => dispatch({ type: 'show-settings-new-style' })} />{styles.length === 0 && <p className="cha-empty-list">No styles configured</p>}{styles.map((style) => (
        <SettingsRow description={`${style.font} · ${style.weight} · ${style.size}`} icon={<CharacterIcon />} key={style.id} label={style.display_name} onClick={() => dispatch({ type: 'inspect-style', styleId: style.id, styleName: style.display_name })} />
      ))}</div>}
    </section>
  );
}

export function NewStyleScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const [name, setName] = useState('');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!name.trim() || saving) return;
    setSaving(true);
    setError(null);
    try {
      const created = await client.createStyle({ display_name: name.trim() });
      dispatch({ type: 'inspect-style', styleId: created.id, styleName: created.display_name });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The style could not be created.'));
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="New style settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-styles' })} type="button"><ChevronLeftIcon /><span>Styles</span></button>
      {sessionReport}
      <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
        <fieldset disabled={saving}><legend>Style details</legend><TransliteratingInput autoFocus className="cha-form-control" id="cha-new-style-name" label="Name" onValueChange={setName} placeholder="e.g. Editorial" value={name} /></fieldset>
        <p className="cha-settings-note">After creation, you can choose typography, size, and color.</p>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving} onClick={() => dispatch({ type: 'show-settings-styles' })} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!name.trim() || saving} type="submit">{saving ? 'Creating…' : 'Create style'}</button></div>
      </form>
    </section>
  );
}

function styleUpdate(detail: StyleDetail): StyleUpdate {
  const { id: _id, used_by: _usedBy, writable: _writable, ...update } = detail;
  return update;
}

interface StyleScreenProps extends SettingsScreenProps {
  reloadVersion?: number;
}

export function StyleScreen({
  client,
  dispatch,
  reloadVersion = 0,
  sessionReport,
  state,
}: StyleScreenProps) {
  const id = state.inspectedStyle.id;
  const [detail, setDetail] = useState<StyleDetail | null>(null);
  const [draft, setDraft] = useState<StyleUpdate | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const [revision, setRevision] = useState(0);
  useEffect(() => {
    let current = true;
    setDetail(null);
    setDraft(null);
    setError(null);
    if (!id) return () => { current = false; };
    void client.listStyles().then(
      (styles) => {
        if (!current) return;
        const loaded = styles.find((style) => style.id === id);
        if (!loaded) return setError('That style was not found.');
        setDetail(loaded);
        setDraft(styleUpdate(loaded));
        dispatch({
          type: 'style-detail-loaded',
          styleId: loaded.id,
          styleName: loaded.display_name,
          writable: loaded.writable,
        });
      },
      (failure: unknown) => { if (current) setError(publicErrorMessage(failure, 'Style settings could not be loaded.')); },
    );
    return () => { current = false; };
  }, [client, dispatch, id, reloadVersion, revision]);

  function change<Key extends keyof StyleUpdate>(key: Key, value: StyleUpdate[Key]) {
    setDraft((current) => current ? { ...current, [key]: value } : current);
  }
  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!id || !draft || saving || deleting) return;
    setSaving(true);
    setError(null);
    try {
      const updated = await client.updateStyle(id, draft);
      setDetail(updated);
      setDraft(styleUpdate(updated));
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Style settings could not be saved.'));
    } finally {
      setSaving(false);
    }
  }
  async function remove() {
    setConfirming(false);
    if (!id || saving || deleting) return;
    setDeleting(true);
    setError(null);
    try {
      await client.deleteStyle(id);
      dispatch({ type: 'show-settings-styles' });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The style could not be deleted.'));
      setDeleting(false);
    }
  }
  const dirty = detail && draft && JSON.stringify(styleUpdate(detail)) !== JSON.stringify(draft);
  const appearance = draft && {
    font: draft.font,
    style: draft.style,
    weight: draft.weight,
    size: draft.size,
    text_color: draft.text_color,
  };
  return (
    <section className="cha-screen cha-navigation" aria-label="Style settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-styles' })} type="button"><ChevronLeftIcon /><span>Styles</span></button>
      {sessionReport}
      {!id && <p className="cha-state-message">No style is selected.</p>}
      {id && !detail && !error && <p className="cha-state-message" role="status">Loading style…</p>}
      {error && !detail && <LoadFailure message={error} retry={() => setRevision((value) => value + 1)} />}
      {detail && draft && (
        <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
          <fieldset aria-label="Style settings" disabled={saving || deleting || !detail.writable}>
            <div className="cha-settings-form-grid">
              <label>Font<select className="cha-form-control" onChange={(event) => change('font', event.target.value as StyleUpdate['font'])} value={draft.font}><option value="sans">Sans</option><option value="serif">Serif</option><option value="mono">Mono</option></select></label>
              <label>Slant<select className="cha-form-control" onChange={(event) => change('style', event.target.value as StyleUpdate['style'])} value={draft.style}><option value="normal">Normal</option><option value="italic">Italic</option></select></label>
              <label>Weight<select className="cha-form-control" onChange={(event) => change('weight', event.target.value as StyleUpdate['weight'])} value={draft.weight}><option value="light">Light</option><option value="normal">Normal</option><option value="medium">Medium</option><option value="semibold">Semibold</option><option value="bold">Bold</option></select></label>
              <label>Size<select className="cha-form-control" onChange={(event) => change('size', event.target.value as StyleUpdate['size'])} value={draft.size}><option value="small">Small</option><option value="normal">Normal</option><option value="large">Large</option></select></label>
              <label>Text color<select className="cha-form-control" onChange={(event) => change('text_color', event.target.value as StyleUpdate['text_color'])} value={draft.text_color}><option value="normal">Normal</option><option value="muted">Muted</option><option value="accent">Accent</option></select></label>
            </div>
          </fieldset>
          <p className={`cha-style-sample cha-message-text${appearance ? voiceClasses(appearance) : ''}`}>The chief task in life is this…</p>
          <UsedBy empty="Nothing uses this style." items={detail.used_by} />
          {error && <p className="cha-error-message" role="alert">{error}</p>}
          <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={!dirty || saving || deleting} onClick={() => setDraft(styleUpdate(detail))} type="button">Reset</button><button className="cha-button cha-button-primary" disabled={!dirty || saving || deleting || !detail.writable} type="submit">{saving ? 'Saving…' : 'Save style'}</button></div>
          <div className="cha-settings-form-actions">
            <button className="cha-button cha-button-danger" disabled={saving || deleting || !detail.writable} onClick={() => setConfirming(true)} type="button">{deleting ? 'Deleting…' : 'Delete style'}</button>
          </div>
        </form>
      )}
      {confirming && (
        <ConfirmDialog
          confirmLabel="Delete style"
          message={`Delete “${detail?.display_name ?? 'this style'}”? This cannot be undone.`}
          onCancel={() => setConfirming(false)}
          onConfirm={() => void remove()}
          title="Delete style?"
        />
      )}
    </section>
  );
}

export function VoicesScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const { data: voices, error, retry } = useLoad(
    client, loadVoices, 'Voices could not be loaded.',
  );
  return (
    <section className="cha-screen cha-navigation" aria-label="Voices settings">
      <div className="cha-detail-toolbar">
        <BackToSettings dispatch={dispatch} />
        <button
          className="cha-detail-link"
          onClick={() => dispatch({ type: 'show-settings-voice-input' })}
          type="button"
        >
          <span>Voice settings</span>
          <ChevronRightIcon />
        </button>
      </div>
      {sessionReport}
      {voices === null && !error && <p className="cha-state-message" role="status">Loading voices…</p>}
      {error && <LoadFailure message={error} retry={retry} />}
      {voices && (
        <div className="cha-list">
          <SettingsRow description="Add a voice for character speech" icon={<PlusIcon />} label="New voice" onClick={() => dispatch({ type: 'show-settings-new-voice' })} />
          {voices.length === 0 && <p className="cha-empty-list">No voices configured</p>}
          {voices.map((voice) => (
            <SettingsRow
              description={voice.description}
              icon={<SpeakerIcon />}
              key={voice.id}
              label={voice.display_name}
              onClick={() => dispatch({
                type: 'inspect-voice',
                voiceId: voice.id,
                voiceName: voice.display_name,
              })}
            />
          ))}
        </div>
      )}
    </section>
  );
}

const defaultVoiceInput: VoiceInputSettings = {
  url: 'https://api.openai.com/v1/realtime/calls',
  model: 'gpt-live-transcribe',
  api_key: '',
  delay: 'low',
  prompt: '',
};

function defaultVoiceOutput(voices: VoiceDetail[]): VoiceOutputSettings {
  return {
    url: 'https://api.fish.audio/v1/tts',
    model: 's2.1-pro',
    api_key: '',
    output_format: 'mp3',
    default_voice: voices[0]?.display_name ?? '',
  };
}

export function VoiceSettingsScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const [savedInput, setSavedInput] =
    useState<VoiceInputSettings | null | undefined>(undefined);
  const [input, setInput] = useState<VoiceInputSettings>(defaultVoiceInput);
  const [savedOutput, setSavedOutput] =
    useState<VoiceOutputSettings | null | undefined>(undefined);
  const [output, setOutput] = useState<VoiceOutputSettings>(defaultVoiceOutput([]));
  const [keys, setKeys] = useState<ApiKeyDetail[] | null>(null);
  const [voices, setVoices] = useState<VoiceDetail[] | null>(null);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const [revision, setRevision] = useState(0);

  useEffect(() => {
    let current = true;
    setSavedInput(undefined);
    setSavedOutput(undefined);
    setKeys(null);
    setVoices(null);
    setError(null);
    void Promise.all([
      client.getVoiceInputSettings(),
      client.getVoiceOutputSettings(),
      client.listApiKeys(),
      client.listVoices(),
    ]).then(
      ([inputSettings, outputSettings, loadedKeys, loadedVoices]) => {
        if (!current) return;
        setSavedInput(inputSettings);
        setInput(inputSettings ?? defaultVoiceInput);
        setSavedOutput(outputSettings);
        setOutput(outputSettings ?? defaultVoiceOutput(loadedVoices));
        setKeys(loadedKeys);
        setVoices(loadedVoices);
      },
      (failure: unknown) => {
        if (current) setError(publicErrorMessage(
          failure, 'Voice settings could not be loaded.',
        ));
      },
    );
    return () => { current = false; };
  }, [client, revision]);

  const inputBaseline = savedInput ?? defaultVoiceInput;
  const outputBaseline = savedOutput ?? defaultVoiceOutput(voices ?? []);
  const dirty = input.url !== inputBaseline.url
    || input.model !== inputBaseline.model
    || input.api_key !== inputBaseline.api_key
    || input.delay !== inputBaseline.delay
    || input.prompt !== inputBaseline.prompt
    || output.url !== outputBaseline.url
    || output.model !== outputBaseline.model
    || output.api_key !== outputBaseline.api_key
    || output.output_format !== outputBaseline.output_format
    || output.default_voice !== outputBaseline.default_voice;

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!input.url.trim() || !input.model.trim() || !input.api_key
      || !output.url.trim() || !output.model.trim() || !output.api_key
      || !output.output_format.trim() || !output.default_voice || saving) return;
    setError(null);
    setMessage(null);
    setSaving(true);
    try {
      const [updatedInput, updatedOutput] = await Promise.all([
        client.saveVoiceInputSettings({
          url: input.url.trim(),
          model: input.model.trim(),
          api_key: input.api_key,
          delay: input.delay,
          prompt: input.prompt,
        }),
        client.saveVoiceOutputSettings({
          url: output.url,
          model: output.model,
          api_key: output.api_key,
          output_format: output.output_format.trim(),
          default_voice: output.default_voice,
        }),
      ]);
      const endpointOriginChanged = savedInput == null
        || new URL(savedInput.url).origin !== new URL(updatedInput.url).origin;
      setSavedInput(updatedInput);
      setInput(updatedInput);
      setSavedOutput(updatedOutput);
      setOutput(updatedOutput);
      setMessage('Voice settings saved.');
      if (endpointOriginChanged) reloadForVoiceSettings();
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Voice settings could not be saved.'));
    } finally {
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="Voice settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-voices' })} type="button"><ChevronLeftIcon /><span>Voices</span></button>
      {sessionReport}
      {(savedInput === undefined || savedOutput === undefined) && !error && <p className="cha-state-message" role="status">Loading voice settings…</p>}
      {error && (savedInput === undefined || savedOutput === undefined) && <LoadFailure message={error} retry={() => setRevision((value) => value + 1)} />}
      {savedInput !== undefined && savedOutput !== undefined && keys && voices && (
        <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
          <label>Input URL endpoint<input autoFocus className="cha-form-control" onChange={(event) => setInput({ ...input, url: event.target.value })} type="url" value={input.url} /></label>
          <label>Input model name<input className="cha-form-control" onChange={(event) => setInput({ ...input, model: event.target.value })} value={input.model} /></label>
          <label>Input API key name<select className="cha-form-control" onChange={(event) => setInput({ ...input, api_key: event.target.value })} value={input.api_key}><option value="">Select an API key</option>{keys.map((key) => <option key={key.id} value={key.id}>{key.display_name}</option>)}</select></label>
          <label>Input delay<select className="cha-form-control" onChange={(event) => setInput({ ...input, delay: event.target.value as VoiceInputSettings['delay'] })} value={input.delay}><option value="low">Low</option><option value="medium">Medium</option><option value="high">High</option><option value="xhigh">Extra high</option></select></label>
          <label>Input prompt<textarea className="cha-form-control" onChange={(event) => setInput({ ...input, prompt: event.target.value })} rows={4} value={input.prompt} /></label>
          <label>Output URL endpoint<input className="cha-form-control" onChange={(event) => setOutput({ ...output, url: event.target.value })} type="url" value={output.url} /></label>
          <label>Output model name<input className="cha-form-control" onChange={(event) => setOutput({ ...output, model: event.target.value })} value={output.model} /></label>
          <label>Output API key name<select className="cha-form-control" onChange={(event) => setOutput({ ...output, api_key: event.target.value })} value={output.api_key}><option value="">Select an API key</option>{keys.map((key) => <option key={key.id} value={key.id}>{key.display_name}</option>)}</select></label>
          <label>Output format<input className="cha-form-control" onChange={(event) => setOutput({ ...output, output_format: event.target.value })} value={output.output_format} /></label>
          <label>Default voice<select className="cha-form-control" onChange={(event) => setOutput({ ...output, default_voice: event.target.value })} value={output.default_voice}><option value="">Select a voice</option>{voices.map((voice) => <option key={voice.id} value={voice.display_name}>{voice.display_name}</option>)}</select></label>
          {keys.length === 0 && <p className="cha-error-message" role="alert">Add an API key before configuring voice.</p>}
          {voices.length === 0 && <p className="cha-error-message" role="alert">Add a voice before configuring voice output.</p>}
          {message && <p className="cha-state-message" role="status">{message}</p>}
          {error && <p className="cha-error-message" role="alert">{error}</p>}
          <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={!dirty || saving} onClick={() => { setInput(inputBaseline); setOutput(outputBaseline); setMessage(null); }} type="button">Reset</button><button className="cha-button cha-button-primary" disabled={!dirty || !input.url.trim() || !input.model.trim() || !input.api_key || !output.url.trim() || !output.model.trim() || !output.api_key || !output.output_format.trim() || !output.default_voice || saving} type="submit">{saving ? 'Saving…' : 'Save voice settings'}</button></div>
        </form>
      )}
    </section>
  );
}

export function NewVoiceScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const [name, setName] = useState('');
  const [description, setDescription] = useState('');
  const [voiceReferenceId, setVoiceReferenceId] = useState('');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!name.trim() || !description.trim() || !voiceReferenceId.trim() || saving) return;
    setSaving(true);
    setError(null);
    try {
      const created = await client.createVoice({
        display_name: name.trim(),
        description: description.trim(),
        elevenlabs_voice_id: voiceReferenceId.trim(),
      });
      dispatch({
        type: 'inspect-voice',
        voiceId: created.id,
        voiceName: created.display_name,
      });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The voice could not be created.'));
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="New voice settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-voices' })} type="button"><ChevronLeftIcon /><span>Voices</span></button>
      {sessionReport}
      <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
        <TransliteratingInput autoFocus className="cha-form-control" disabled={saving} id="cha-new-voice-name" label="Name" onValueChange={setName} placeholder="e.g. Brian" value={name} />
        <label htmlFor="cha-new-voice-description">Description<textarea className="cha-form-control cha-voice-description" disabled={saving} id="cha-new-voice-description" onChange={(event) => setDescription(event.target.value)} placeholder="Describe how this voice sounds" value={description} /></label>
        <label htmlFor="cha-new-voice-reference-id">Voice ID<input className="cha-form-control" disabled={saving} id="cha-new-voice-reference-id" onChange={(event) => setVoiceReferenceId(event.target.value)} value={voiceReferenceId} /></label>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving} onClick={() => dispatch({ type: 'show-settings-voices' })} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!name.trim() || !description.trim() || !voiceReferenceId.trim() || saving} type="submit">{saving ? 'Registering…' : 'Register voice'}</button></div>
      </form>
    </section>
  );
}

function voiceUpdate(detail: VoiceDetail): VoiceUpdate {
  const { id: _id, used_by: _usedBy, writable: _writable, ...update } = detail;
  return update;
}

function optionalNumber(value: string): number | null {
  return value === '' ? null : Number(value);
}

interface VoiceScreenProps extends SettingsScreenProps {
  reloadVersion?: number;
}

export function VoiceScreen({
  client,
  dispatch,
  reloadVersion = 0,
  sessionReport,
  state,
}: VoiceScreenProps) {
  const id = state.inspectedVoice.id;
  const [detail, setDetail] = useState<VoiceDetail | null>(null);
  const [draft, setDraft] = useState<VoiceUpdate | null>(null);
  const [previewText, setPreviewText] = useState(
    'The chief task in life is simply this: to identify and separate matters so that I can say clearly to myself which are externals not under my control.',
  );
  const [previewing, setPreviewing] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [previewError, setPreviewError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const [revision, setRevision] = useState(0);
  const preview = useRef<TextToSpeechSession | null>(null);
  const speechConfiguration = useTextToSpeechConfiguration(client);

  useEffect(() => () => preview.current?.stop(), []);
  useEffect(() => {
    let current = true;
    preview.current?.stop();
    preview.current = null;
    setPreviewing(false);
    setDetail(null);
    setDraft(null);
    setError(null);
    if (!id) return () => { current = false; };
    void client.listVoices().then(
      (voices) => {
        if (!current) return;
        const loaded = voices.find((voice) => voice.id === id);
        if (!loaded) return setError('That voice was not found.');
        setDetail(loaded);
        setDraft(voiceUpdate(loaded));
        dispatch({
          type: 'voice-detail-loaded',
          voiceId: loaded.id,
          voiceName: loaded.display_name,
          writable: loaded.writable,
        });
      },
      (failure: unknown) => {
        if (current) setError(publicErrorMessage(failure, 'Voice settings could not be loaded.'));
      },
    );
    return () => { current = false; };
  }, [client, dispatch, id, reloadVersion, revision]);

  function change<Key extends keyof VoiceUpdate>(key: Key, value: VoiceUpdate[Key]) {
    setDraft((current) => current ? { ...current, [key]: value } : current);
  }

  function stopPreview() {
    preview.current?.stop();
    preview.current = null;
    setPreviewing(false);
  }

  async function togglePreview() {
    if (previewing) return stopPreview();
    if (!draft || !speechConfiguration || !previewText.trim()
      || !draft.elevenlabs_voice_id.trim()) return;
    setPreviewError(null);
    const session = new TextToSpeechSession(
      speechConfiguration,
      speechVoice(draft),
      previewText.trim(),
      () => {
        if (preview.current === session) preview.current = null;
        setPreviewing(false);
      },
      undefined,
      undefined,
      undefined,
      nativeSpeechFromClient(client),
    );
    preview.current = session;
    setPreviewing(true);
    try {
      await session.play();
    } catch (failure: unknown) {
      if (preview.current !== session) return;
      session.stop();
      preview.current = null;
      setPreviewing(false);
      setPreviewError(failure instanceof TextToSpeechError
        ? failure.message : 'Voice preview could not be played.');
    }
  }

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!id || !detail || !draft || saving || deleting) return;
    setSaving(true);
    setError(null);
    try {
      const updated = await client.updateVoice(id, draft);
      setDetail(updated);
      setDraft(voiceUpdate(updated));
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Voice settings could not be saved.'));
    } finally {
      setSaving(false);
    }
  }

  async function remove() {
    setConfirming(false);
    if (!id || saving || deleting) return;
    setDeleting(true);
    setError(null);
    try {
      await client.deleteVoice(id);
      dispatch({ type: 'show-settings-voices' });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The voice could not be deleted.'));
      setDeleting(false);
    }
  }

  const dirty = detail && draft
    && JSON.stringify(voiceUpdate(detail)) !== JSON.stringify(draft);
  const disabled = saving || deleting || !detail?.writable;
  return (
    <section className="cha-screen cha-navigation" aria-label="Voice settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-voices' })} type="button"><ChevronLeftIcon /><span>Voices</span></button>
      {sessionReport}
      {!id && <p className="cha-state-message">No voice is selected.</p>}
      {id && !detail && !error && <p className="cha-state-message" role="status">Loading voice…</p>}
      {error && !detail && <LoadFailure message={error} retry={() => setRevision((value) => value + 1)} />}
      {detail && draft && (
        <form className="cha-settings-form cha-voice-settings-form" onSubmit={(event) => void save(event)}>
          <label htmlFor="cha-voice-description">Description<textarea className="cha-form-control cha-voice-description" disabled={disabled} id="cha-voice-description" onChange={(event) => change('description', event.target.value)} value={draft.description} /></label>
          <label htmlFor="cha-voice-reference-id">Voice ID<input className="cha-form-control" disabled={disabled} id="cha-voice-reference-id" onChange={(event) => change('elevenlabs_voice_id', event.target.value)} value={draft.elevenlabs_voice_id} /></label>
          <h2 className="cha-settings-section-title">Delivery</h2>
          <div className="cha-settings-form-grid">
            <label>Speed <span>0.7–1.2</span><input className="cha-form-control" disabled={disabled} max="1.2" min="0.7" onChange={(event) => change('speed', optionalNumber(event.target.value))} step="0.01" type="number" value={draft.speed ?? ''} /></label>
          </div>
          <h2 className="cha-settings-section-title">Preview</h2>
          <label htmlFor="cha-voice-preview-text">Text to speak<textarea className="cha-form-control cha-voice-preview-text" id="cha-voice-preview-text" onChange={(event) => setPreviewText(event.target.value)} value={previewText} /></label>
          {previewError && <p className="cha-error-message" role="alert">{previewError}</p>}
          {speechConfiguration && <div className="cha-settings-form-actions"><button className="cha-button cha-voice-preview-action" disabled={!previewing && (!previewText.trim() || !draft.elevenlabs_voice_id.trim())} onClick={() => void togglePreview()} type="button">{previewing ? <><StopIcon /> Stop preview</> : <><SpeakerIcon /> Play preview</>}</button></div>}
          <UsedBy empty="Nothing uses this voice." items={detail.used_by} />
          {error && <p className="cha-error-message" role="alert">{error}</p>}
          <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={!dirty || saving || deleting} onClick={() => setDraft(voiceUpdate(detail))} type="button">Reset</button><button className="cha-button cha-button-primary" disabled={!dirty || disabled || !draft.elevenlabs_voice_id.trim()} type="submit">{saving ? 'Saving…' : 'Save voice'}</button></div>
          <div className="cha-settings-form-actions"><button className="cha-button cha-button-danger" disabled={saving || deleting || !detail.writable} onClick={() => setConfirming(true)} type="button">{deleting ? 'Deleting…' : 'Delete voice'}</button></div>
        </form>
      )}
      {confirming && (
        <ConfirmDialog confirmLabel="Delete voice" message={`Delete “${detail?.display_name ?? 'this voice'}”? This cannot be undone.`} onCancel={() => setConfirming(false)} onConfirm={() => void remove()} title="Delete voice?" />
      )}
    </section>
  );
}

export function ApiKeysScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const { data: keys, error, retry } = useLoad(
    client, loadApiKeys, 'API keys could not be loaded.',
  );
  return (
    <section className="cha-screen cha-navigation" aria-label="API keys settings">
      <BackToSettings dispatch={dispatch} />
      {sessionReport}
      {keys === null && !error && <p className="cha-state-message" role="status">Loading API keys…</p>}
      {error && <LoadFailure message={error} retry={retry} />}
      {keys && <div className="cha-list"><SettingsRow description="Configure database upload and download" icon={<DatabaseIcon />} label="R2 storage" onClick={() => dispatch({ type: 'show-settings-r2-storage' })} /><SettingsRow description="Save a model-service secret in this vault" icon={<PlusIcon />} label="New API key" onClick={() => dispatch({ type: 'show-settings-new-api-key' })} />{keys.length === 0 && <p className="cha-empty-list">No model API keys saved</p>}{keys.map((key) => <SettingsRow description={key.used_by.length ? `Used by ${key.used_by.join(', ')}` : 'Saved in this vault · Not in use'} icon={<KeyIcon />} key={key.id} label={key.display_name} onClick={() => dispatch({ type: 'inspect-api-key', apiKeyId: key.id, apiKeyName: key.display_name })} />)}</div>}
    </section>
  );
}

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

export function R2StorageScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
  const [detail, setDetail] = useState<R2StorageDetail | null | undefined>(undefined);
  const [draft, setDraft] = useState<R2Draft>(() => r2Draft(null));
  const [busy, setBusy] = useState<'save' | 'delete' | null>(null);
  const [confirming, setConfirming] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [revision, setRevision] = useState(0);

  useEffect(() => {
    let current = true;
    setDetail(undefined);
    setError(null);
    void client.getR2Storage().then(
      (loaded) => {
        if (!current) return;
        setDetail(loaded);
        setDraft(r2Draft(loaded));
      },
      (failure: unknown) => {
        if (current) setError(publicErrorMessage(
          failure, 'R2 storage credentials could not be loaded.',
        ));
      },
    );
    return () => { current = false; };
  }, [client, revision]);

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
      dispatch({ type: 'show-settings-api-keys' });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'R2 storage credentials could not be removed.'));
      setBusy(null);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="R2 storage settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-api-keys' })} type="button"><ChevronLeftIcon /><span>API Keys</span></button>
      {sessionReport}
      {detail === undefined && !error && <p className="cha-state-message" role="status">Loading R2 storage credentials…</p>}
      {error && detail === undefined && <LoadFailure message={error} retry={() => setRevision((value) => value + 1)} />}
      {detail !== undefined && <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
        <fieldset disabled={busy !== null}>
          <label>R2 URL<input autoComplete="off" autoFocus className="cha-form-control" onChange={(event) => change('url', event.target.value)} placeholder="https://account.r2.cloudflarestorage.com/bucket" type="url" value={draft.url} /></label>
          <label>Access key ID<input autoComplete="off" className="cha-form-control" onChange={(event) => change('access_key_id', event.target.value)} placeholder="Paste access key ID" value={draft.access_key_id} /></label>
          <label>Secret key<input autoComplete="off" className="cha-form-control" onChange={(event) => change('secret_key', event.target.value)} placeholder={detail ? 'Leave blank to keep the current secret' : 'Paste secret key'} type="password" value={draft.secret_key} /></label>
        </fieldset>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={!dirty || busy !== null} onClick={() => setDraft(r2Draft(detail))} type="button">Reset</button><button className="cha-button cha-button-primary" disabled={!dirty || !valid || busy !== null} type="submit">{busy === 'save' ? 'Saving…' : 'Save R2 credentials'}</button></div>
        {detail && <div className="cha-settings-form-actions"><button className="cha-button cha-button-danger" disabled={busy !== null} onClick={() => setConfirming(true)} type="button">{busy === 'delete' ? 'Removing…' : 'Remove R2 credentials'}</button></div>}
      </form>}
      {confirming && <ConfirmDialog confirmLabel="Remove R2 credentials" message="Remove R2 storage credentials from this vault? Database upload and download will stop working." onCancel={() => setConfirming(false)} onConfirm={() => void remove()} title="Remove R2 credentials?" />}
    </section>
  );
}

export function NewApiKeyScreen({ client, dispatch, sessionReport }: SettingsScreenProps) {
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
      dispatch({ type: 'inspect-api-key', apiKeyId: created.id, apiKeyName: created.display_name });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The API key could not be saved.'));
      setSaving(false);
    }
  }
  return (
    <section className="cha-screen cha-navigation" aria-label="New API key settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-api-keys' })} type="button"><ChevronLeftIcon /><span>API Keys</span></button>
      {sessionReport}
      <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
        <fieldset disabled={saving}><legend>Key details</legend><TransliteratingInput autoFocus autoComplete="off" className="cha-form-control" id="cha-new-api-key-name" label="Name" onValueChange={setName} placeholder="e.g. OpenRouter" value={name} /><label>API key<input autoComplete="off" className="cha-form-control" onChange={(event) => setValue(event.target.value)} placeholder="Paste key" type="password" value={value} /></label></fieldset>
        <p className="cha-settings-note">The value is stored in this vault and is never returned to the browser.</p>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving} onClick={() => dispatch({ type: 'show-settings-api-keys' })} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!name.trim() || !value || saving} type="submit">{saving ? 'Saving…' : 'Save API key'}</button></div>
      </form>
    </section>
  );
}

export function ApiKeyScreen({ client, dispatch, sessionReport, state }: SettingsScreenProps) {
  const id = state.inspectedApiKey.id;
  const [key, setKey] = useState<ApiKeyDetail | null>(null);
  const [replacement, setReplacement] = useState('');
  const [busy, setBusy] = useState<'value' | 'delete' | null>(null);
  const [confirming, setConfirming] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [revision, setRevision] = useState(0);
  useEffect(() => {
    let current = true;
    setKey(null);
    setError(null);
    if (!id) return () => { current = false; };
    void client.listApiKeys().then(
      (keys) => {
        if (!current) return;
        const loaded = keys.find((candidate) => candidate.id === id);
        if (!loaded) return setError('That API key was not found.');
        setKey(loaded);
        dispatch({
          type: 'api-key-detail-loaded',
          apiKeyId: loaded.id,
          apiKeyName: loaded.display_name,
        });
      },
      (failure: unknown) => { if (current) setError(publicErrorMessage(failure, 'The API key could not be loaded.')); },
    );
    return () => { current = false; };
  }, [client, dispatch, id, revision]);

  async function replace(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!id || !replacement || busy) return;
    setBusy('value'); setError(null);
    try { const updated = await client.replaceApiKeyValue(id, replacement); setKey(updated); setReplacement(''); }
    catch (failure: unknown) { setError(publicErrorMessage(failure, 'The API key value could not be replaced.')); }
    finally { setBusy(null); }
  }
  async function remove() {
    setConfirming(false);
    if (!id || busy) return;
    setBusy('delete'); setError(null);
    try { await client.deleteApiKey(id); dispatch({ type: 'show-settings-api-keys' }); }
    catch (failure: unknown) { setError(publicErrorMessage(failure, 'The API key could not be removed.')); setBusy(null); }
  }
  return (
    <section className="cha-screen cha-navigation" aria-label="API key settings">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-settings-api-keys' })} type="button"><ChevronLeftIcon /><span>API Keys</span></button>
      {sessionReport}
      {!id && <p className="cha-state-message">No API key is selected.</p>}
      {id && !key && !error && <p className="cha-state-message" role="status">Loading API key…</p>}
      {error && !key && <LoadFailure message={error} retry={() => setRevision((value) => value + 1)} />}
      {key && <form className="cha-settings-form" onSubmit={(event) => void replace(event)}>
        <fieldset disabled={busy !== null}><label>New API key<input autoComplete="off" className="cha-form-control" onChange={(event) => setReplacement(event.target.value)} placeholder="Paste replacement key" type="password" value={replacement} /></label><div className="cha-settings-form-actions"><button className="cha-button cha-button-primary" disabled={!replacement || busy !== null} type="submit">{busy === 'value' ? 'Saving…' : 'Save'}</button></div></fieldset>
        <UsedBy empty="Nothing references this key." items={key.used_by} />
        {error && <p className="cha-error-message" role="alert">{error}</p>}
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
