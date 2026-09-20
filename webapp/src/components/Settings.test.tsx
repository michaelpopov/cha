import { act, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { ChaError, type ChaClient, type ProviderDetail, type StyleDetail, type VaultDetail } from '../api/client';
import { appReducer, initialAppState, type AppAction, type AppState } from '../state/view';
import { bootstrapFixture, fixtureClient, voiceDetailFixture } from '../test/fixtures';
import {
  ApiKeyScreen,
  ApiKeysScreen,
  DownloadVaultScreen,
  MergeVaultScreen,
  NewApiKeyScreen,
  NewProviderScreen,
  NewStyleScreen,
  NewVaultScreen,
  NewVoiceScreen,
  ProviderScreen,
  ProvidersScreen,
  R2StorageScreen,
  SettingsNavigation,
  StyleScreen,
  StylesScreen,
  VaultScreen,
  VaultsScreen,
  VoiceScreen,
  VoiceSettingsScreen,
  VoicesScreen,
} from './Settings';
import { TopBar } from './TopBar';

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

const provider: ProviderDetail = {
  id: 'router',
  display_name: 'OpenRouter',
  host: 'openrouter.ai',
  port: 443,
  base_path: '/api',
  mode: 'net',
  model: 'openai/gpt-5',
  stream: true,
  temperature: null,
  max_tokens: null,
  timeout_s: 600,
  idle_timeout_s: 60,
  api_key: null,
  reasoning_effort: '',
  reasoning_format: 'auto',
  https: true,
  api: 'chat_completions',
  auth: 'none',
  web_search: 'off',
  cache_retention: 'short',
  openrouter_targets: [],
  writable: true,
  used_by: ['Guide'],
};

const style: StyleDetail = {
  id: 'editorial',
  display_name: 'Editorial',
  font: 'serif',
  style: 'normal',
  weight: 'bold',
  size: 'normal',
  text_color: 'normal',
  writable: true,
  used_by: ['Guide'],
};

const vaults: VaultDetail[] = [
  {
    display_name: 'Personal',
    protected: false,
    data_path: '/data/personal.sqlite3',
    mirror_path: null,
    modify_path: '/work/personal',
    active: true,
    can_delete: false,
  },
  {
    display_name: 'Projects',
    protected: false,
    data_path: '/data/projects.sqlite3',
    mirror_path: '/mirror/projects',
    modify_path: null,
    active: false,
    can_delete: true,
  },
];

describe('Settings screens', () => {
  it.each([
    ['Vaults', VaultsScreen, 'listVaults', 'New vault'],
    ['Providers', ProvidersScreen, 'listProviders', 'No providers configured'],
    ['Styles', StylesScreen, 'listStyles', 'No styles configured'],
    ['Voices', VoicesScreen, 'listVoices', 'No voices configured'],
    ['API keys', ApiKeysScreen, 'listApiKeys', 'No model API keys saved'],
    ['Merge', MergeVaultScreen, 'listVaults', 'No other vaults'],
  ] as const)('retries a failed %s list load', async (_name, Screen, method, loadedText) => {
    const load = vi.fn(async () => [])
      .mockRejectedValueOnce(new ChaError('application_unavailable', 'Please try again.'));
    const client = fixtureClient({ [method]: load });
    const { rerender } = render(
      <Screen client={client} dispatch={vi.fn()} sessionReport={null} state={initialAppState} />,
    );

    expect(await screen.findByRole('alert')).toHaveTextContent('Please try again.');
    await userEvent.click(screen.getByRole('button', { name: 'Try again' }));
    expect(await screen.findByText(loadedText)).toBeInTheDocument();
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    rerender(
      <Screen client={client} dispatch={vi.fn()} sessionReport={null} state={initialAppState} />,
    );
    expect(load).toHaveBeenCalledTimes(2);
  });

  it('shows Vaults above Providers, Styles, Voices, and API Keys', async () => {
    const dispatch = vi.fn();
    render(<SettingsNavigation dispatch={dispatch} />);

    const destinations = screen.getAllByRole('button');
    expect(destinations[0]).toHaveAccessibleName(/Vaults/);
    expect(screen.getByRole('button', { name: /Providers/ })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: /Styles/ })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: /Voices/ })).toBeInTheDocument();
    await userEvent.click(screen.getByRole('button', { name: /API Keys/ }));
    expect(dispatch).toHaveBeenCalledWith({ type: 'show-settings-api-keys' });
  });

  it('shows vault status privately and opens download and merge', async () => {
    const dispatch = vi.fn();
    render(
      <VaultsScreen
        client={fixtureClient({ listVaults: async () => vaults })}
        dispatch={dispatch}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    const activeVault = await screen.findByRole('button', { name: /Personal/ });
    expect(within(activeVault).getByText('Active')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Projects' })).toBeInTheDocument();
    expect(screen.queryByText('/data/personal.sqlite3')).not.toBeInTheDocument();
    expect(screen.queryByText('/data/projects.sqlite3')).not.toBeInTheDocument();

    const merge = screen.getByRole('button', { name: /Merge into active vault/ });
    const operations = screen.getAllByRole('button');
    expect(operations.map((button) => button.textContent)).toEqual(expect.arrayContaining([
      expect.stringMatching(/New vault/),
      expect.stringMatching(/Download vault/),
      expect.stringMatching(/Merge into active vault/),
    ]));
    await userEvent.click(merge);
    expect(dispatch).toHaveBeenCalledWith({ type: 'show-settings-merge-vault' });
    await userEvent.click(screen.getByRole('button', { name: /Download vault/ }));
    expect(dispatch).toHaveBeenCalledWith({ type: 'show-settings-download-vault' });
  });

  it('downloads an R2 vault from its row without showing the file extension', async () => {
    const downloaded: VaultDetail = {
      display_name: 'Archive',
      protected: false,
      data_path: '/data/Archive.sqlite3',
      mirror_path: null,
      modify_path: null,
      active: false,
      can_delete: true,
    };
    const downloadR2Vault = vi.fn(async (name: string) => ({
      ...downloaded,
      display_name: name,
      data_path: `/data/${name}.sqlite3`,
    }));
    const dispatch = vi.fn();
    render(
      <DownloadVaultScreen
        client={fixtureClient({
          listR2Vaults: async () => ['Archive', 'Travel'],
          downloadR2Vault,
        })}
        dispatch={dispatch}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    expect(await screen.findByRole('button', { name: 'Archive' })).toBeInTheDocument();
    expect(screen.queryByText(/\.sqlite3/)).not.toBeInTheDocument();
    await userEvent.click(screen.getByRole('button', { name: 'Archive' }));

    expect(downloadR2Vault).toHaveBeenCalledWith('Archive');
    expect(dispatch).toHaveBeenCalledWith({ type: 'vault-downloaded', vault: downloaded });
    expect(await screen.findByText('Downloaded')).toBeInTheDocument();

    await userEvent.click(screen.getByRole('button', { name: 'Travel' }));
    expect(downloadR2Vault).toHaveBeenCalledWith('Travel');
    expect(await screen.findAllByText('Downloaded')).toHaveLength(2);
    expect(screen.getByRole('button', { name: /Archive/ })).toBeDisabled();
    expect(screen.getByRole('button', { name: /Travel/ })).toBeDisabled();
  });

  async function confirmMerge() {
    await userEvent.click(await screen.findByRole('button', { name: 'Merge' }));
    const dialog = within(await screen.findByRole('dialog'));
    expect(dialog.getByRole('heading', { name: 'Merge vault?' })).toBeInTheDocument();
    expect(dialog.getByText(/Merge “Projects” into “Personal”/)).toBeInTheDocument();
    expect(dialog.getByText(/overwrite destination files at matching paths/i)).toBeInTheDocument();
    expect(dialog.getByText(/cannot be undone/i)).toBeInTheDocument();
    await userEvent.click(dialog.getByRole('button', { name: 'Merge' }));
  }

  function mergeState(): AppState {
    return appReducer(
      appReducer(initialAppState, { type: 'bootstrap-loaded', bootstrap: bootstrapFixture }),
      { type: 'show-settings-merge-vault' },
    );
  }

  it('excludes the active vault from merge sources and keeps it active after success', async () => {
    const mergeVault = vi.fn(async () => undefined);
    let state = mergeState();
    const dispatch = (action: AppAction) => {
      state = appReducer(state, action);
    };
    render(
      <MergeVaultScreen
        client={fixtureClient({ listVaults: async () => vaults, mergeVault })}
        dispatch={dispatch}
        sessionReport={null}
        state={state}
      />,
    );

    const source = await screen.findByLabelText('Source vault');
    expect(source).toHaveValue('');
    expect(screen.getByRole('button', { name: 'Merge' })).toBeDisabled();
    expect(screen.queryByRole('option', { name: 'Personal' })).not.toBeInTheDocument();
    expect(screen.getByRole('option', { name: 'Projects' })).toBeInTheDocument();

    await userEvent.selectOptions(source, 'Projects');
    await confirmMerge();

    expect(mergeVault).toHaveBeenCalledWith('Projects', undefined);
    expect(await screen.findByText('Merge complete')).toBeInTheDocument();
    expect(state.bootstrap?.vault_name).toBe('Personal');
    expect(state.mainView).toBe('settings-merge-vault');
  });

  it('offers a reload only when a merge changes a voice endpoint origin', async () => {
    let inputUrl = 'https://api.openai.com/v1/realtime/calls';
    const mergeVault = vi.fn(async () => undefined);
    render(
      <MergeVaultScreen
        client={fixtureClient({
          listVaults: async () => vaults,
          mergeVault,
          getVoiceInputSettings: async () => ({
            url: inputUrl,
            model: 'transcribe',
            api_key: 'api_key_1',
            delay: 'low', prompt: '',
          }),
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    await userEvent.selectOptions(await screen.findByLabelText('Source vault'), 'Projects');
    await confirmMerge();
    expect(await screen.findByText('Merge complete')).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Reload' })).not.toBeInTheDocument();

    mergeVault.mockImplementationOnce(async () => {
      inputUrl = 'https://voice.example/v1/text-to-speech';
    });
    await confirmMerge();
    expect(await screen.findByRole('button', { name: 'Reload' })).toBeInTheDocument();
    expect(screen.getByText('Reload CHA to use the merged voice settings.')).toBeInTheDocument();
  });

  it.each(['failed read', 'invalid URL'])('merges and offers a reload when inspecting original voice settings encounters %s', async (failure) => {
    const mergeVault = vi.fn(async () => undefined);
    const getVoiceInputSettings = vi.fn<ChaClient['getVoiceInputSettings']>(async () => null);
    if (failure === 'failed read') {
      getVoiceInputSettings.mockRejectedValueOnce(new Error('Voice settings could not be loaded.'));
    } else {
      getVoiceInputSettings.mockImplementationOnce(async () => ({
        url: 'https://voice.example:bad',
        model: 'transcribe',
        api_key: 'api_key_1',
        delay: 'low', prompt: '',
      }));
    }
    render(
      <MergeVaultScreen
        client={fixtureClient({ listVaults: async () => vaults, mergeVault, getVoiceInputSettings })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    await userEvent.selectOptions(await screen.findByLabelText('Source vault'), 'Projects');
    await confirmMerge();

    expect(await screen.findByText('Merge complete')).toBeInTheDocument();
    expect(mergeVault).toHaveBeenCalledWith('Projects', undefined);
    expect(screen.getByRole('button', { name: 'Reload' })).toBeInTheDocument();
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });

  it('shows an empty merge state when there is no other vault', async () => {
    render(
      <MergeVaultScreen
        client={fixtureClient({ listVaults: async () => [vaults[0]] })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    expect(await screen.findByText('No other vaults')).toBeInTheDocument();
    expect(screen.queryByLabelText('Source vault')).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Merge' })).not.toBeInTheDocument();
  });

  it('prevents a second merge request while one is pending', async () => {
    let finish: (() => void) | undefined;
    const mergeVault = vi.fn(() => new Promise<void>((resolve) => {
      finish = resolve;
    }));
    render(
      <MergeVaultScreen
        client={fixtureClient({ listVaults: async () => vaults, mergeVault })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    await userEvent.selectOptions(await screen.findByLabelText('Source vault'), 'Projects');
    await confirmMerge();
    expect(await screen.findByRole('button', { name: 'Merging…' })).toBeDisabled();
    expect(mergeVault).toHaveBeenCalledTimes(1);
    finish?.();
    expect(await screen.findByText('Merge complete')).toBeInTheDocument();
  });

  it('asks for the source password and retries until the merge succeeds', async () => {
    const mergeVault = vi.fn(async (_source: string, password?: string) => {
      if (password !== 'secret') {
        throw new ChaError(
          'source_vault_password_required',
          'Password required to merge this vault',
        );
      }
    });
    render(
      <MergeVaultScreen
        client={fixtureClient({ listVaults: async () => vaults, mergeVault })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    await userEvent.selectOptions(await screen.findByLabelText('Source vault'), 'Projects');
    await confirmMerge();
    expect(await screen.findByRole('heading', { name: 'Open Projects' })).toBeInTheDocument();
    expect(mergeVault).toHaveBeenLastCalledWith('Projects', undefined);

    await userEvent.type(screen.getByLabelText('Password'), 'wrong');
    await userEvent.click(screen.getByRole('button', { name: 'Open vault' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('Password required to merge this vault');
    expect(mergeVault).toHaveBeenLastCalledWith('Projects', 'wrong');
    expect(screen.getByLabelText('Source vault')).toHaveValue('Projects');

    const password = screen.getByLabelText('Password');
    await userEvent.clear(password);
    await userEvent.type(password, 'secret');
    await userEvent.click(screen.getByRole('button', { name: 'Open vault' }));
    expect(await screen.findByText('Merge complete')).toBeInTheDocument();
    expect(mergeVault).toHaveBeenLastCalledWith('Projects', 'secret');
    expect(screen.queryByRole('heading', { name: 'Open Projects' })).not.toBeInTheDocument();
  });

  it('sends no retry when the source password dialog is cancelled', async () => {
    const mergeVault = vi.fn(async () => {
      throw new ChaError(
        'source_vault_password_required',
        'Password required to merge this vault',
      );
    });
    render(
      <MergeVaultScreen
        client={fixtureClient({ listVaults: async () => vaults, mergeVault })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    await userEvent.selectOptions(await screen.findByLabelText('Source vault'), 'Projects');
    await confirmMerge();
    expect(await screen.findByRole('heading', { name: 'Open Projects' })).toBeInTheDocument();
    await userEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(screen.queryByRole('heading', { name: 'Open Projects' })).not.toBeInTheDocument();
    expect(mergeVault).toHaveBeenCalledTimes(1);
    expect(mergeVault).toHaveBeenCalledWith('Projects', undefined);
  });

  it('keeps a validation failure visible so merge can be retried', async () => {
    const mergeVault = vi.fn(async () => {
      throw new ChaError('invalid_argument', 'Duplicate character id “guide”.');
    });
    render(
      <MergeVaultScreen
        client={fixtureClient({ listVaults: async () => vaults, mergeVault })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    await userEvent.selectOptions(await screen.findByLabelText('Source vault'), 'Projects');
    await confirmMerge();
    expect(await screen.findByRole('alert')).toHaveTextContent('Duplicate character id “guide”.');
    expect(screen.getByRole('button', { name: 'Merge' })).toBeEnabled();
    await confirmMerge();
    expect(mergeVault).toHaveBeenCalledTimes(2);
    expect(await screen.findByRole('alert')).toHaveTextContent('Duplicate character id “guide”.');
  });

  it('creates a copied vault without activating it', async () => {
    const created: VaultDetail = {
      display_name: 'Archive',
      protected: false,
      data_path: '/data/archive.sqlite3',
      mirror_path: '/mirror/archive',
      modify_path: null,
      active: false,
      can_delete: true,
    };
    const createVault = vi.fn(async () => created);
    const dispatch = vi.fn();
    render(
      <NewVaultScreen
        client={fixtureClient({ listVaults: async () => vaults, createVault })}
        dispatch={dispatch}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    await userEvent.type(await screen.findByLabelText('Display name'), 'Archive');
    await userEvent.selectOptions(screen.getByLabelText('Initial database'), 'Projects');
    expect(screen.queryByLabelText('Database path')).not.toBeInTheDocument();
    expect(screen.queryByText('Vault details')).not.toBeInTheDocument();
    expect(screen.queryByLabelText(/Mirror path/)).not.toBeInTheDocument();
    expect(screen.queryByLabelText(/Modify path/)).not.toBeInTheDocument();
    expect(screen.queryByText(/active vault does not change/i)).not.toBeInTheDocument();
    await userEvent.click(screen.getByRole('button', { name: 'Create vault' }));

    expect(createVault).toHaveBeenCalledWith({
      display_name: 'Archive',
      copy_from: 'Projects',
      password: null,
    });
    expect(dispatch).toHaveBeenCalledWith({ type: 'vault-created', vault: created });
  });

  it('creates an empty vault when no database is selected', async () => {
    const created: VaultDetail = {
      display_name: 'Empty',
      protected: false,
      data_path: '/data/empty.sqlite3',
      mirror_path: null,
      modify_path: null,
      active: false,
      can_delete: true,
    };
    const createVault = vi.fn(async () => created);
    const dispatch = vi.fn();
    render(
      <NewVaultScreen
        client={fixtureClient({ listVaults: async () => vaults, createVault })}
        dispatch={dispatch}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    await userEvent.type(await screen.findByLabelText('Display name'), 'Empty');
    expect(screen.getByLabelText('Initial database')).toHaveValue('');
    await userEvent.click(screen.getByRole('button', { name: 'Create vault' }));

    expect(createVault).toHaveBeenCalledWith({
      display_name: 'Empty',
      copy_from: null,
      password: null,
    });
    expect(dispatch).toHaveBeenCalledWith({ type: 'vault-created', vault: created });
  });

  it('creates a password-protected vault', async () => {
    const created: VaultDetail = {
      display_name: 'Private',
      protected: true,
      data_path: '/data/private.sqlite3',
      mirror_path: null,
      modify_path: null,
      active: false,
      can_delete: true,
    };
    const createVault = vi.fn(async () => created);
    render(
      <NewVaultScreen
        client={fixtureClient({ listVaults: async () => vaults, createVault })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    await userEvent.type(await screen.findByLabelText('Display name'), 'Private');
    await userEvent.click(screen.getByLabelText('Protected vault'));
    const password = screen.getByLabelText('Password');
    expect(password).toHaveAttribute('type', 'password');
    await userEvent.click(screen.getByRole('button', { name: 'Show password' }));
    expect(password).toHaveAttribute('type', 'text');
    await userEvent.click(screen.getByRole('button', { name: 'Hide password' }));
    expect(password).toHaveAttribute('type', 'password');
    expect(screen.getByRole('button', { name: 'Create vault' })).toBeDisabled();
    await userEvent.type(password, 'secret');
    await userEvent.click(screen.getByRole('button', { name: 'Create vault' }));
    expect(createVault).toHaveBeenCalledWith({
      display_name: 'Private', copy_from: null, password: 'secret',
    });
  });

  it('adds password protection to an existing vault', async () => {
    const updateVault = vi.fn(async () => ({ ...vaults[1], protected: true }));
    render(
      <VaultScreen
        client={fixtureClient({ listVaults: async () => vaults, updateVault })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={{ ...initialAppState, inspectedVaultName: 'Projects' }}
      />,
    );

    await userEvent.click(await screen.findByLabelText('Protected vault'));
    await userEvent.type(screen.getByLabelText('Password'), 'secret');
    await userEvent.click(screen.getByRole('button', { name: 'Protect vault' }));
    expect(updateVault).toHaveBeenCalledWith('Projects', {
      display_name: 'Projects', password: 'secret',
    });
  });

  it('removes an inactive vault without exposing its name or path fields', async () => {
    const deleteVault = vi.fn(async () => undefined);
    const dispatch = vi.fn();
    render(
      <VaultScreen
        client={fixtureClient({
          listVaults: async () => vaults,
          deleteVault,
        })}
        dispatch={dispatch}
        sessionReport={null}
        state={{ ...initialAppState, inspectedVaultName: 'Projects' }}
      />,
    );

    await screen.findByLabelText('Protected vault');
    expect(screen.queryByLabelText('Display name')).not.toBeInTheDocument();
    expect(screen.queryByLabelText('Database path')).not.toBeInTheDocument();
    expect(screen.queryByText('Vault details')).not.toBeInTheDocument();
    expect(screen.queryByLabelText(/Mirror path/)).not.toBeInTheDocument();
    expect(screen.queryByLabelText(/Modify path/)).not.toBeInTheDocument();

    await userEvent.click(screen.getByRole('button', { name: 'Delete vault' }));
    expect(screen.getByText(/database and folders will be kept/)).toBeInTheDocument();
    await userEvent.click(within(screen.getByRole('dialog')).getByRole(
      'button', { name: 'Delete vault' },
    ));
    expect(deleteVault).toHaveBeenCalledWith('Projects');
    expect(dispatch).toHaveBeenCalledWith({
      type: 'vault-deleted', vaultName: 'Projects',
    });
  });

  it('creates an API key without retaining its value on screen', async () => {
    const createApiKey = vi.fn(async ({ display_name }: { display_name: string }) => ({
      id: 'api_key_1', display_name, has_value: true, used_by: [],
    }));
    const dispatch = vi.fn();
    render(
      <NewApiKeyScreen
        client={fixtureClient({ createApiKey })}
        dispatch={dispatch}
        sessionReport={null}
        state={initialAppState}
      />,
    );
    await userEvent.type(screen.getByLabelText('Name'), 'OpenRouter');
    await userEvent.type(screen.getByLabelText('API key'), 'sk-secret');
    await userEvent.click(screen.getByRole('button', { name: 'Save API key' }));

    expect(createApiKey).toHaveBeenCalledWith({
      display_name: 'OpenRouter', value: 'sk-secret',
    });
    expect(dispatch).toHaveBeenCalledWith({
      type: 'inspect-api-key', apiKeyId: 'api_key_1', apiKeyName: 'OpenRouter',
    });
    expect(screen.getByLabelText('API key')).toHaveValue('');
  });

  it('creates R2 credentials without retaining the secret on screen', async () => {
    const saveR2Storage = vi.fn(async ({ display_name, url, access_key_id }) => ({
      id: 'api_key_1', display_name, url, access_key_id, has_secret_key: true,
    }));
    render(
      <R2StorageScreen
        client={fixtureClient({ getR2Storage: async () => null, saveR2Storage })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    const r2Url = await screen.findByLabelText('R2 URL');
    expect(screen.queryByLabelText('Name')).not.toBeInTheDocument();
    expect(screen.queryByText('R2 credentials')).not.toBeInTheDocument();
    expect(screen.queryByText(/These credentials are stored/)).not.toBeInTheDocument();
    await userEvent.type(r2Url, 'https://account.example/bucket');
    await userEvent.type(screen.getByLabelText('Access key ID'), 'access-id');
    await userEvent.type(screen.getByLabelText('Secret key'), 'private-secret');
    await userEvent.click(screen.getByRole('button', { name: 'Save R2 credentials' }));

    expect(saveR2Storage).toHaveBeenCalledWith({
      display_name: 'R2',
      url: 'https://account.example/bucket',
      access_key_id: 'access-id',
      secret_key: 'private-secret',
    });
    expect(screen.getByLabelText('Secret key')).toHaveValue('');
  });

  it('updates R2 metadata without replacing its secret and can remove it', async () => {
    const detail = {
      id: 'api_key_4',
      display_name: 'Backups',
      url: 'https://old.example/bucket',
      access_key_id: 'access-id',
      has_secret_key: true,
    };
    const saveR2Storage = vi.fn(async (request) => ({
      ...detail,
      ...request,
      has_secret_key: true,
    }));
    const deleteR2Storage = vi.fn(async () => undefined);
    const dispatch = vi.fn();
    render(
      <R2StorageScreen
        client={fixtureClient({
          getR2Storage: async () => detail,
          saveR2Storage,
          deleteR2Storage,
        })}
        dispatch={dispatch}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    const url = await screen.findByLabelText('R2 URL');
    await userEvent.clear(url);
    await userEvent.type(url, 'https://new.example/bucket');
    await userEvent.click(screen.getByRole('button', { name: 'Save R2 credentials' }));
    expect(saveR2Storage).toHaveBeenCalledWith({
      display_name: 'Backups',
      url: 'https://new.example/bucket',
      access_key_id: 'access-id',
      secret_key: null,
    });

    await userEvent.click(screen.getByRole('button', { name: 'Remove R2 credentials' }));
    await userEvent.click(within(screen.getByRole('dialog')).getByRole(
      'button', { name: 'Remove R2 credentials' },
    ));
    expect(deleteR2Storage).toHaveBeenCalledOnce();
    expect(dispatch).toHaveBeenCalledWith({ type: 'show-settings-api-keys' });
  });

  it('stores a saved-key reference in a provider', async () => {
    const updateProvider = vi.fn(async (_id, update) => ({
      id: 'router', used_by: provider.used_by, writable: true, ...update,
    }));
    render(
      <ProviderScreen
        client={fixtureClient({
          getProvider: async () => provider,
          listApiKeys: async () => [{
            id: 'api_key_1', display_name: 'Router key', has_value: true, used_by: [],
          }],
          updateProvider,
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedProvider: { ...initialAppState.inspectedProvider, id: 'router' },
        }}
      />,
    );
    await userEvent.selectOptions(await screen.findByLabelText('Credentials'), 'api_key_1');
    await userEvent.click(screen.getByRole('button', { name: 'Save changes' }));

    expect(updateProvider).toHaveBeenCalledWith(
      'router',
      expect.objectContaining({ api_key: 'api_key_1' }),
    );
  });

  it('creates a provider and opens its settings', async () => {
    const createProvider = vi.fn(async ({ display_name }: { display_name: string }) => ({
      ...provider, id: 'provider_1', display_name,
    }));
    const dispatch = vi.fn();
    render(
      <NewProviderScreen
        client={fixtureClient({ createProvider })}
        dispatch={dispatch}
        sessionReport={null}
        state={initialAppState}
      />,
    );
    await userEvent.type(await screen.findByLabelText('Name'), 'OpenRouter');
    await userEvent.click(screen.getByRole('button', { name: 'Create provider' }));

    expect(createProvider).toHaveBeenCalledWith({
      display_name: 'OpenRouter', copy_from: null,
    });
    expect(dispatch).toHaveBeenCalledWith({
      type: 'inspect-provider', providerId: 'provider_1', providerName: 'OpenRouter',
    });
  });

  it('creates a provider by copying existing settings', async () => {
    const createProvider = vi.fn(async ({ display_name }: { display_name: string }) => ({
      ...provider, id: 'provider_1', display_name,
    }));
    render(
      <NewProviderScreen
        client={fixtureClient({
          createProvider,
          listProviders: async () => [{
            id: provider.id,
            display_name: provider.display_name,
            model: provider.model,
            host: provider.host,
          }],
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    await userEvent.type(await screen.findByLabelText('Name'), 'OpenRouter copy');
    await userEvent.selectOptions(screen.getByLabelText('Initial settings'), provider.id);
    await userEvent.click(screen.getByRole('button', { name: 'Create provider' }));

    expect(createProvider).toHaveBeenCalledWith({
      display_name: 'OpenRouter copy', copy_from: provider.id,
    });
  });

  it('saves ordered OpenRouter inference targets', async () => {
    const configured = {
      ...provider,
      openrouter_targets: ['CoreWeave'],
    };
    const updateProvider = vi.fn(async (_id, update) => ({
      id: configured.id, used_by: configured.used_by, writable: true, ...update,
    }));
    render(
      <ProviderScreen
        client={fixtureClient({
          getProvider: async () => configured,
          listApiKeys: async () => [],
          updateProvider,
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedProvider: { ...initialAppState.inspectedProvider, id: configured.id },
        }}
      />,
    );

    const targets = await screen.findByLabelText('Inference targets');
    expect(targets).toHaveValue('CoreWeave');
    await userEvent.clear(targets);
    await userEvent.type(targets, 'CoreWeave{enter}Crusoe');
    await userEvent.click(screen.getByRole('button', { name: 'Save changes' }));

    expect(updateProvider).toHaveBeenCalledWith(configured.id, expect.objectContaining({
      openrouter_targets: ['CoreWeave', 'Crusoe'],
    }));
  });

  it('tests unsaved provider settings without saving them', async () => {
    const testProvider = vi.fn(async () => undefined);
    const updateProvider = vi.fn();
    render(
      <ProviderScreen
        client={fixtureClient({
          getProvider: async () => provider,
          listApiKeys: async () => [],
          testProvider,
          updateProvider,
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedProvider: { ...initialAppState.inspectedProvider, id: provider.id },
        }}
      />,
    );

    expect(await screen.findByLabelText('Model')).toHaveValue('openai/gpt-5');
    expect(screen.getByLabelText('Base URL')).toHaveValue('https://openrouter.ai/api');
    expect(screen.getByLabelText('API format')).toHaveValue('chat_completions');
    expect(screen.getByLabelText('Credentials')).toBeInTheDocument();
    const targets = screen.getByLabelText('Inference targets');
    expect(targets).toHaveValue('');
    expect(screen.queryByLabelText('Name')).not.toBeInTheDocument();
    expect(screen.queryByLabelText('Host')).not.toBeInTheDocument();
    expect(screen.queryByLabelText('Port')).not.toBeInTheDocument();
    expect(screen.queryByLabelText('Environment variable')).not.toBeInTheDocument();
    expect(screen.queryByLabelText('Reasoning effort')).not.toBeInTheDocument();
    expect(screen.getByText('Used by')).toBeInTheDocument();
    expect(screen.getByText('Guide')).toBeInTheDocument();

    await userEvent.type(screen.getByLabelText('Model'), '-candidate');
    await userEvent.click(await screen.findByRole('button', { name: 'Test' }));

    expect(testProvider).toHaveBeenCalledWith(provider.id, expect.objectContaining({
      model: 'openai/gpt-5-candidate',
      reasoning_effort: '',
    }));
    expect(updateProvider).not.toHaveBeenCalled();
    expect(await screen.findByText('Provider responded successfully.')).toBeInTheDocument();
  });

  it('preserves the scheme and custom port represented by Base URL', async () => {
    const localProvider: ProviderDetail = {
      ...provider,
      host: '127.0.0.1',
      port: 11434,
      https: false,
      base_path: '/openai',
      reasoning_effort: 'low',
    };
    const updateProvider = vi.fn(async (_id, update) => ({
      id: localProvider.id, used_by: localProvider.used_by, writable: true, ...update,
    }));
    render(
      <ProviderScreen
        client={fixtureClient({
          getProvider: async () => localProvider,
          listApiKeys: async () => [],
          updateProvider,
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedProvider: { ...initialAppState.inspectedProvider, id: provider.id },
        }}
      />,
    );

    expect(await screen.findByLabelText('Base URL'))
      .toHaveValue('http://127.0.0.1:11434/openai');
    expect(screen.queryByLabelText('Inference targets')).not.toBeInTheDocument();
    const model = screen.getByLabelText('Model');
    await userEvent.clear(model);
    await userEvent.type(model, 'local-model');
    await userEvent.click(screen.getByRole('button', { name: 'Save changes' }));

    expect(updateProvider).toHaveBeenCalledWith(localProvider.id, expect.objectContaining({
      host: '127.0.0.1',
      port: 11434,
      base_path: '/openai',
      https: false,
      mode: 'net',
      stream: true,
      web_search: 'off',
      reasoning_effort: '',
      auth: 'none',
      timeout_s: 600,
      reasoning_format: 'auto',
    }));
  });

  it('preserves OpenAI OAuth as an explicit credential choice', async () => {
    const oauthProvider: ProviderDetail = {
      ...provider,
      id: 'chatgpt',
      display_name: 'ChatGPT',
      host: 'chatgpt.com',
      base_path: '/backend-api/codex',
      api: 'responses',
      auth: 'openai_subscription',
      cache_retention: 'off',
    };
    const updateProvider = vi.fn(async (_id, update) => ({
      id: oauthProvider.id, used_by: oauthProvider.used_by, writable: true, ...update,
    }));
    render(
      <ProviderScreen
        client={fixtureClient({
          getProvider: async () => oauthProvider,
          listApiKeys: async () => [],
          updateProvider,
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedProvider: { ...initialAppState.inspectedProvider, id: oauthProvider.id },
        }}
      />,
    );

    expect(await screen.findByLabelText('Credentials')).toHaveDisplayValue('OpenAI OAuth');
    await userEvent.clear(screen.getByLabelText('Model'));
    await userEvent.type(screen.getByLabelText('Model'), 'gpt-5.6-sol');
    await userEvent.click(screen.getByRole('button', { name: 'Save changes' }));

    expect(updateProvider).toHaveBeenCalledWith(oauthProvider.id, expect.objectContaining({
      auth: 'openai_subscription',
      api_key: null,
      host: 'chatgpt.com',
      base_path: '/backend-api/codex',
      api: 'responses',
    }));
  });

  it('rejects OpenAI OAuth for a non-OpenAI OAuth endpoint', async () => {
    const updateProvider = vi.fn();
    render(
      <ProviderScreen
        client={fixtureClient({
          getProvider: async () => provider,
          listApiKeys: async () => [],
          updateProvider,
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedProvider: { ...initialAppState.inspectedProvider, id: provider.id },
        }}
      />,
    );

    await userEvent.selectOptions(
      await screen.findByLabelText('Credentials'),
      screen.getByRole('option', { name: 'OpenAI OAuth' }),
    );
    await userEvent.click(screen.getByRole('button', { name: 'Save changes' }));

    expect(screen.getByRole('alert')).toHaveTextContent(
      'OpenAI OAuth requires Base URL https://chatgpt.com/backend-api/codex.',
    );
    expect(updateProvider).not.toHaveBeenCalled();
  });

  it('renames the provider from the shared editable title', async () => {
    const updateProvider = vi.fn(async (_id, update) => ({
      id: provider.id, used_by: provider.used_by, writable: true, ...update,
    }));
    const dispatch = vi.fn();
    const onProviderUpdated = vi.fn();
    const state = {
      ...initialAppState,
      mainView: 'settings-provider' as const,
      inspectedProvider: {
        id: provider.id,
        name: provider.display_name,
        writable: true,
      },
    };
    render(
      <TopBar
        client={fixtureClient({
          getProvider: async () => provider,
          updateProvider,
        })}
        dispatch={dispatch}
        onDeleteCharacter={vi.fn()}
        onDeleteForum={vi.fn()}
        onDeletePersona={vi.fn()}
        onCharacterDefinitionUpdated={vi.fn()}
        onForumDefinitionUpdated={vi.fn()}
        onPersonaDefinitionUpdated={vi.fn()}
        onProviderUpdated={onProviderUpdated}
        onStyleUpdated={vi.fn()}
        onVoiceUpdated={vi.fn()}
        state={state}
        title={provider.display_name}
      />,
    );

    await userEvent.click(screen.getByRole('button', { name: 'Rename OpenRouter' }));
    const name = screen.getByLabelText('Provider name');
    await userEvent.clear(name);
    await userEvent.type(name, 'Router');
    await userEvent.click(screen.getByRole('button', { name: 'Save provider name' }));

    expect(updateProvider).toHaveBeenCalledWith(provider.id, expect.objectContaining({
      display_name: 'Router',
    }));
    expect(dispatch).toHaveBeenCalledWith({
      type: 'provider-updated',
      providerId: provider.id,
      providerName: 'Router',
      writable: true,
    });
    expect(onProviderUpdated).toHaveBeenCalledOnce();
  });

  it('creates a style and opens its settings', async () => {
    const createStyle = vi.fn(async ({ display_name }: { display_name: string }) => ({
      ...style, id: 'style_1', display_name,
    }));
    const dispatch = vi.fn();
    render(
      <NewStyleScreen
        client={fixtureClient({ createStyle })}
        dispatch={dispatch}
        sessionReport={null}
        state={initialAppState}
      />,
    );
    await userEvent.type(screen.getByLabelText('Name'), 'Quiet');
    await userEvent.click(screen.getByRole('button', { name: 'Create style' }));

    expect(createStyle).toHaveBeenCalledWith({ display_name: 'Quiet' });
    expect(dispatch).toHaveBeenCalledWith({
      type: 'inspect-style', styleId: 'style_1', styleName: 'Quiet',
    });
  });

  it('shows style settings without a name field or explanatory note', async () => {
    render(
      <StyleScreen
        client={fixtureClient({ listStyles: async () => [style] })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedStyle: { ...initialAppState.inspectedStyle, id: style.id },
        }}
      />,
    );

    expect(await screen.findByLabelText('Font')).toBeInTheDocument();
    expect(screen.queryByLabelText('Name')).not.toBeInTheDocument();
    expect(screen.queryByText(/Saving restarts sessions/)).not.toBeInTheDocument();
    expect(screen.queryByText('Style')).not.toBeInTheDocument();
    expect(screen.getByText('Used by')).toBeInTheDocument();
    expect(screen.getByText('Guide')).toBeInTheDocument();
  });

  it('renames a style from the shared editable title', async () => {
    const updateStyle = vi.fn(async (_id, update) => ({
      id: style.id, used_by: style.used_by, writable: true, ...update,
    }));
    const dispatch = vi.fn();
    const onStyleUpdated = vi.fn();
    const state = {
      ...initialAppState,
      mainView: 'settings-style' as const,
      inspectedStyle: {
        id: style.id,
        name: style.display_name,
        writable: true,
      },
    };
    render(
      <TopBar
        client={fixtureClient({
          listStyles: async () => [style],
          updateStyle,
        })}
        dispatch={dispatch}
        onDeleteCharacter={vi.fn()}
        onDeleteForum={vi.fn()}
        onDeletePersona={vi.fn()}
        onCharacterDefinitionUpdated={vi.fn()}
        onForumDefinitionUpdated={vi.fn()}
        onPersonaDefinitionUpdated={vi.fn()}
        onProviderUpdated={vi.fn()}
        onStyleUpdated={onStyleUpdated}
        onVoiceUpdated={vi.fn()}
        state={state}
        title={style.display_name}
      />,
    );

    await userEvent.click(screen.getByRole('button', { name: 'Rename Editorial' }));
    const name = screen.getByLabelText('Style name');
    await userEvent.clear(name);
    await userEvent.type(name, 'Narrative');
    await userEvent.click(screen.getByRole('button', { name: 'Save style name' }));

    expect(updateStyle).toHaveBeenCalledWith(style.id, expect.objectContaining({
      display_name: 'Narrative',
    }));
    expect(dispatch).toHaveBeenCalledWith({
      type: 'style-updated',
      styleId: style.id,
      styleName: 'Narrative',
      writable: true,
    });
    expect(onStyleUpdated).toHaveBeenCalledOnce();
  });

  it('lists each voice by its name and description only', async () => {
    const dispatch = vi.fn();
    render(
      <VoicesScreen
        client={fixtureClient({ listVoices: async () => [voiceDetailFixture] })}
        dispatch={dispatch}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    const brian = await screen.findByRole('button', { name: /Brian/ });
    expect(brian).toHaveTextContent('Deep, resonant, comforting');
    expect(brian).not.toHaveTextContent(voiceDetailFixture.elevenlabs_voice_id);
    expect(brian).not.toHaveTextContent('settings');
    await userEvent.click(screen.getByRole('button', { name: 'Voice settings' }));
    expect(dispatch).toHaveBeenCalledWith({ type: 'show-settings-voice-input' });
  });

  it('loads and saves vault-backed voice settings', async () => {
    const saveVoiceInputSettings = vi.fn(async (settings) => settings);
    const saveVoiceOutputSettings = vi.fn(async (settings) => settings);
    render(
      <VoiceSettingsScreen
        client={fixtureClient({
          getVoiceInputSettings: async () => ({
            url: 'https://api.openai.com/v1/realtime/calls',
            model: 'gpt-live-transcribe',
            api_key: 'api_key_1',
            delay: 'medium',
            prompt: 'Software design discussion.',
          }),
          listApiKeys: async () => [{
            id: 'api_key_1',
            display_name: 'OpenAI',
            has_value: true,
            used_by: ['Voice input'],
          }, {
            id: 'api_key_2',
            display_name: 'FishAudio',
            has_value: true,
            used_by: ['Voice output'],
          }],
          listVoices: async () => [voiceDetailFixture],
          getVoiceOutputSettings: async () => ({
            url: 'https://api.fish.audio/v1/tts',
            model: 's2.1-pro',
            api_key: 'api_key_2',
            output_format: 'mp3',
            default_voice: 'Brian',
          }),
          saveVoiceInputSettings,
          saveVoiceOutputSettings,
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    expect(await screen.findByLabelText('Input URL endpoint')).toHaveValue(
      'https://api.openai.com/v1/realtime/calls',
    );
    expect(screen.getByLabelText('Input API key name')).toHaveDisplayValue('OpenAI');
    expect(screen.getByLabelText('Input delay')).toHaveDisplayValue('Medium');
    expect(screen.getByLabelText('Input prompt')).toHaveValue('Software design discussion.');
    expect(screen.getByLabelText('Output API key name')).toHaveDisplayValue('FishAudio');
    expect(screen.getByLabelText('Default voice')).toHaveDisplayValue('Brian');
    const model = screen.getByLabelText('Input model name');
    await userEvent.clear(model);
    await userEvent.type(model, 'next-transcribe-model');
    await userEvent.selectOptions(screen.getByLabelText('Input delay'), 'xhigh');
    await userEvent.clear(screen.getByLabelText('Input prompt'));
    await userEvent.type(screen.getByLabelText('Input prompt'), 'Names and technical terms.');
    await userEvent.clear(screen.getByLabelText('Output format'));
    await userEvent.type(screen.getByLabelText('Output format'), 'opus');
    await userEvent.click(screen.getByRole('button', { name: 'Save voice settings' }));

    expect(saveVoiceInputSettings).toHaveBeenCalledWith({
      url: 'https://api.openai.com/v1/realtime/calls',
      model: 'next-transcribe-model',
      api_key: 'api_key_1',
      delay: 'xhigh',
      prompt: 'Names and technical terms.',
    });
    expect(saveVoiceOutputSettings).toHaveBeenCalledWith({
      url: 'https://api.fish.audio/v1/tts',
      model: 's2.1-pro',
      api_key: 'api_key_2',
      output_format: 'opus',
      default_voice: 'Brian',
    });
    expect(await screen.findByText('Voice settings saved.')).toBeInTheDocument();
  });

  it('uses the server-normalized settings and server URL errors', async () => {
    const saveVoiceInputSettings = vi.fn(async (settings) => settings);
    const saveVoiceOutputSettings = vi.fn(async (settings) => {
      if (settings.url.startsWith('http:')) {
        throw new ChaError('invalid_argument', 'FishAudio requires an HTTPS URL.');
      }
      return { ...settings, url: 'https://api.fish.audio/v1/tts', model: settings.model.trim() };
    });
    render(<VoiceSettingsScreen
      client={fixtureClient({
        listVoices: async () => [voiceDetailFixture],
        listApiKeys: async () => [{ id: 'key', display_name: 'Key', has_value: true, used_by: [] }],
        getVoiceInputSettings: async () => ({
          url: 'https://api.openai.com/v1/realtime/calls', model: 'gpt-live-transcribe',
          api_key: 'key', delay: 'low', prompt: '',
        }),
        getVoiceOutputSettings: async () => ({
          url: 'https://api.fish.audio/v1/tts', model: 's2.1-pro-free',
          api_key: 'key', output_format: 'mp3', default_voice: voiceDetailFixture.display_name,
        }),
        saveVoiceInputSettings, saveVoiceOutputSettings,
      })}
      dispatch={vi.fn()} sessionReport={null} state={initialAppState}
    />);
    const endpoint = await screen.findByLabelText('Output URL endpoint');
    await userEvent.clear(endpoint);
    await userEvent.type(endpoint, 'HTTPS://API.FISH.AUDIO:443');
    await userEvent.tab();
    const model = screen.getByLabelText('Output model name');
    expect(model).toHaveValue('s2.1-pro-free');
    await userEvent.clear(model);
    await userEvent.type(model, ' custom/model ');
    await userEvent.click(screen.getByRole('button', { name: 'Save voice settings' }));
    expect(await screen.findByText('Voice settings saved.')).toBeInTheDocument();
    expect(saveVoiceOutputSettings).toHaveBeenCalledWith(expect.objectContaining({
      url: 'HTTPS://API.FISH.AUDIO:443', model: ' custom/model ',
    }));
    expect(endpoint).toHaveValue('https://api.fish.audio/v1/tts');

    await userEvent.clear(endpoint);
    await userEvent.type(endpoint, 'http://api.fish.audio');
    await userEvent.click(screen.getByRole('button', { name: 'Save voice settings' }));
    expect(await screen.findByText('FishAudio requires an HTTPS URL.')).toBeInTheDocument();
    expect(saveVoiceInputSettings).toHaveBeenCalledTimes(2);
    expect(saveVoiceOutputSettings).toHaveBeenCalledTimes(2);
  });

  it('registers a voice and opens its editor', async () => {
    const createVoice = vi.fn(async () => voiceDetailFixture);
    const dispatch = vi.fn();
    render(
      <NewVoiceScreen
        client={fixtureClient({ createVoice })}
        dispatch={dispatch}
        sessionReport={null}
        state={initialAppState}
      />,
    );

    await userEvent.type(screen.getByLabelText('Name'), 'Brian');
    await userEvent.type(screen.getByLabelText('Description'), 'Deep, resonant, comforting');
    await userEvent.type(
      screen.getByLabelText('Voice ID'),
      voiceDetailFixture.elevenlabs_voice_id,
    );
    await userEvent.click(screen.getByRole('button', { name: 'Register voice' }));

    expect(createVoice).toHaveBeenCalledWith({
      display_name: 'Brian',
      description: 'Deep, resonant, comforting',
      elevenlabs_voice_id: voiceDetailFixture.elevenlabs_voice_id,
    });
    expect(dispatch).toHaveBeenCalledWith({
      type: 'inspect-voice', voiceId: 'brian', voiceName: 'Brian',
    });
  });

  it('edits a voice in the flat form and keeps preview text separate', async () => {
    const updateVoice = vi.fn(async (_id, update) => ({
      ...voiceDetailFixture,
      ...update,
    }));
    render(
      <VoiceScreen
        client={fixtureClient({
          listVoices: async () => [voiceDetailFixture],
          updateVoice,
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedVoice: { ...initialAppState.inspectedVoice, id: 'brian' },
        }}
      />,
    );

    const description = await screen.findByLabelText('Description');
    expect(screen.getByRole('spinbutton', { name: /Speed/ })).toBeInTheDocument();
    expect(screen.queryByLabelText('Name')).not.toBeInTheDocument();
    expect(screen.queryByText('Voice details')).not.toBeInTheDocument();
    expect(screen.queryByText('Voice source')).not.toBeInTheDocument();
    expect(screen.queryByText(/Blank settings keep/)).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Play preview' }))
      .not.toBeInTheDocument();

    await userEvent.clear(description);
    await userEvent.type(description, 'Warm, captivating storyteller');
    const previewText = screen.getByLabelText('Text to speak');
    await userEvent.clear(previewText);
    await userEvent.type(previewText, 'A custom preview sentence.');
    await userEvent.click(screen.getByRole('button', { name: 'Save voice' }));

    expect(updateVoice).toHaveBeenCalledWith('brian', {
      display_name: 'Brian',
      description: 'Warm, captivating storyteller',
      elevenlabs_voice_id: voiceDetailFixture.elevenlabs_voice_id,
      speed: 0.95,
    });
  });

  it('allows saving settings for a legacy voice without a description', async () => {
    const legacyVoice = { ...voiceDetailFixture, description: '' };
    const updateVoice = vi.fn(async (_id, update) => ({
      ...legacyVoice,
      ...update,
    }));
    render(
      <VoiceScreen
        client={fixtureClient({
          listVoices: async () => [legacyVoice],
          getVoiceOutputRuntime: async () => ({
            url: 'https://api.fish.audio/v1/tts', model: 's2.1-pro',
            output_format: 'mp3', default_voice_id: 'voice',
          }),
          updateVoice,
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedVoice: { ...initialAppState.inspectedVoice, id: 'brian' },
        }}
      />,
    );

    const speed = await screen.findByRole('spinbutton', { name: /Speed/ });
    await userEvent.clear(speed);
    await userEvent.type(speed, '0.9');
    const save = screen.getByRole('button', { name: 'Save voice' });
    expect(save).toBeEnabled();
    await userEvent.click(save);

    expect(updateVoice).toHaveBeenCalledWith('brian', expect.objectContaining({
      description: '', speed: 0.9,
    }));
  });

  it('previews the unsaved voice settings and editable text', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValue(
      new Response(new TextEncoder().encode('audio')),
    );
    const previewSpeech = vi.fn(async () => ({
      url: '/media/preview', resource_id: 'preview', mime_type: 'audio/mpeg', byte_length: 5,
    }));
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return {
        addEventListener: vi.fn(),
        pause: vi.fn(),
        play: vi.fn().mockResolvedValue(undefined),
      };
    }));
    render(
      <VoiceScreen
        client={fixtureClient({
          previewSpeech,
          listVoices: async () => [voiceDetailFixture],
          getVoiceOutputRuntime: async () => ({
            url: 'https://api.fish.audio/v1/tts',
            model: 's2.1-pro',
            output_format: 'mp3',
            default_voice_id: 'fallback',
          }),
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedVoice: { ...initialAppState.inspectedVoice, id: 'brian' },
        }}
      />,
    );

    expect(await screen.findByLabelText('Voice ID')).toHaveValue(voiceDetailFixture.elevenlabs_voice_id);
    expect(screen.getByRole('spinbutton', { name: /Speed/ })).toBeInTheDocument();
    expect(screen.queryByRole('spinbutton', { name: /Stability/ })).not.toBeInTheDocument();
    expect(screen.queryByRole('spinbutton', { name: /Similarity/ })).not.toBeInTheDocument();
    expect(screen.queryByRole('spinbutton', { name: /Style exaggeration/ })).not.toBeInTheDocument();
    expect(screen.queryByLabelText('Speaker boost')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Play preview' })).toBeEnabled();

    const voiceId = screen.getByLabelText('Voice ID');
    await userEvent.clear(voiceId);
    expect(screen.getByRole('button', { name: 'Play preview' })).toBeDisabled();
    await userEvent.type(voiceId, 'unsaved-id');
    const previewText = screen.getByLabelText('Text to speak');
    await userEvent.clear(previewText);
    await userEvent.type(previewText, 'Read this draft.');
    await userEvent.click(screen.getByRole('button', { name: 'Play preview' }));

    expect(previewSpeech).toHaveBeenCalledWith(
      'Read this draft.',
      'unsaved-id',
      { speed: 0.95 },
      expect.any(AbortSignal),
    );
    expect(fetchMock).toHaveBeenCalledWith('/media/preview', expect.any(Object));
  });

  it('renames a voice from the shared editable title', async () => {
    const updateVoice = vi.fn(async (_id, update) => ({
      ...voiceDetailFixture,
      ...update,
    }));
    const dispatch = vi.fn();
    const onVoiceUpdated = vi.fn();
    render(
      <TopBar
        client={fixtureClient({
          listVoices: async () => [voiceDetailFixture],
          updateVoice,
        })}
        dispatch={dispatch}
        onDeleteCharacter={vi.fn()}
        onDeleteForum={vi.fn()}
        onDeletePersona={vi.fn()}
        onCharacterDefinitionUpdated={vi.fn()}
        onForumDefinitionUpdated={vi.fn()}
        onPersonaDefinitionUpdated={vi.fn()}
        onProviderUpdated={vi.fn()}
        onStyleUpdated={vi.fn()}
        onVoiceUpdated={onVoiceUpdated}
        state={{
          ...initialAppState,
          mainView: 'settings-voice',
          inspectedVoice: {
            id: 'brian',
            name: 'Brian',
            writable: true,
          },
        }}
        title="Brian"
      />,
    );

    await userEvent.click(screen.getByRole('button', { name: 'Rename Brian' }));
    const name = screen.getByLabelText('Voice name');
    await userEvent.clear(name);
    await userEvent.type(name, 'George');
    await userEvent.click(screen.getByRole('button', { name: 'Save voice name' }));

    expect(updateVoice).toHaveBeenCalledWith('brian', expect.objectContaining({
      display_name: 'George',
    }));
    expect(dispatch).toHaveBeenCalledWith({
      type: 'voice-updated',
      voiceId: 'brian',
      voiceName: 'George',
      writable: true,
    });
    expect(onVoiceUpdated).toHaveBeenCalledOnce();
  });

  it('deletes an unused provider from its settings screen', async () => {
    const deleteProvider = vi.fn(async () => undefined);
    const dispatch = vi.fn();
    render(
      <ProviderScreen
        client={fixtureClient({
          getProvider: async () => provider,
          listApiKeys: async () => [],
          deleteProvider,
        })}
        dispatch={dispatch}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedProvider: { ...initialAppState.inspectedProvider, id: provider.id },
        }}
      />,
    );
    await userEvent.click(await screen.findByRole('button', { name: 'Delete provider' }));

    // The confirmation is asked in the page: WKWebView shows no window.confirm
    // panel, so a native prompt would leave the button dead.
    const dialog = within(await screen.findByRole('dialog'));
    expect(dialog.getByText(/Delete “OpenRouter”\?/)).toBeInTheDocument();
    await userEvent.click(dialog.getByRole('button', { name: 'Delete provider' }));

    expect(deleteProvider).toHaveBeenCalledWith(provider.id);
    expect(dispatch).toHaveBeenCalledWith({ type: 'show-settings-providers' });
  });

  it('leaves a provider alone when the confirmation is cancelled', async () => {
    const deleteProvider = vi.fn(async () => undefined);
    render(
      <ProviderScreen
        client={fixtureClient({
          getProvider: async () => provider,
          listApiKeys: async () => [],
          deleteProvider,
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedProvider: { ...initialAppState.inspectedProvider, id: provider.id },
        }}
      />,
    );
    await userEvent.click(await screen.findByRole('button', { name: 'Delete provider' }));
    const dialog = within(await screen.findByRole('dialog'));
    await userEvent.click(dialog.getByRole('button', { name: 'Cancel' }));

    expect(deleteProvider).not.toHaveBeenCalled();
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it('deletes an unused style from its settings screen', async () => {
    const deleteStyle = vi.fn(async () => undefined);
    const dispatch = vi.fn();
    render(
      <StyleScreen
        client={fixtureClient({
          listStyles: async () => [style],
          deleteStyle,
        })}
        dispatch={dispatch}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedStyle: { ...initialAppState.inspectedStyle, id: style.id },
        }}
      />,
    );
    await userEvent.click(await screen.findByRole('button', { name: 'Delete style' }));
    const dialog = within(await screen.findByRole('dialog'));
    await userEvent.click(dialog.getByRole('button', { name: 'Delete style' }));

    expect(deleteStyle).toHaveBeenCalledWith(style.id);
    expect(dispatch).toHaveBeenCalledWith({ type: 'show-settings-styles' });
  });

  it('lists locally saved keys with provider usage', async () => {
    const dispatch = vi.fn();
    render(
      <ApiKeysScreen
        client={fixtureClient({ listApiKeys: async () => [{
          id: 'api_key_1', display_name: 'Google', has_value: true, used_by: ['Gemini'],
        }] })}
        dispatch={dispatch}
        sessionReport={null}
        state={initialAppState}
      />,
    );
    expect(await screen.findByRole('button', { name: /Google/ })).toHaveTextContent('Used by Gemini');
    await userEvent.click(screen.getByRole('button', { name: /R2 storage/ }));
    expect(dispatch).toHaveBeenCalledWith({ type: 'show-settings-r2-storage' });
  });

  it('removes a saved API key after an in-page confirmation', async () => {
    const deleteApiKey = vi.fn(async () => undefined);
    const dispatch = vi.fn();
    render(
      <ApiKeyScreen
        client={fixtureClient({
          listApiKeys: async () => [{
            id: 'api_key_1', display_name: 'Google', has_value: true, used_by: [],
          }],
          deleteApiKey,
        })}
        dispatch={dispatch}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedApiKey: {
            id: 'api_key_1',
            name: 'Google',
          },
        }}
      />,
    );
    await userEvent.click(await screen.findByRole('button', { name: 'Remove API key' }));
    const dialog = within(await screen.findByRole('dialog'));
    await userEvent.click(dialog.getByRole('button', { name: 'Remove API key' }));

    expect(deleteApiKey).toHaveBeenCalledWith('api_key_1');
    expect(dispatch).toHaveBeenCalledWith({ type: 'show-settings-api-keys' });
  });

  it('shows the replacement field, provider usage, and save and remove actions for an API key', async () => {
    const replaceApiKeyValue = vi.fn(async () => ({
      id: 'api_key_1', display_name: 'Google', has_value: true, used_by: ['Gemini'],
    }));
    render(
      <ApiKeyScreen
        client={fixtureClient({
          listApiKeys: async () => [{
            id: 'api_key_1', display_name: 'Google', has_value: true, used_by: ['Gemini'],
          }],
          replaceApiKeyValue,
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={{
          ...initialAppState,
          inspectedApiKey: {
            id: 'api_key_1',
            name: 'Google',
          },
        }}
      />,
    );

    const value = await screen.findByLabelText('New API key');
    expect(screen.queryByLabelText('Name')).not.toBeInTheDocument();
    expect(screen.queryByText('Saved locally')).not.toBeInTheDocument();
    expect(screen.getByText('Used by')).toBeInTheDocument();
    expect(screen.getByText('Gemini')).toBeInTheDocument();
    expect(screen.queryByText('Replace value')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Remove API key' })).toBeInTheDocument();

    await userEvent.type(value, 'replacement-secret');
    await userEvent.click(screen.getByRole('button', { name: 'Save' }));
    expect(replaceApiKeyValue).toHaveBeenCalledWith('api_key_1', 'replacement-secret');
  });

  it('renames an API key from the shared editable title', async () => {
    const renameApiKey = vi.fn(async (_id, displayName) => ({
      id: 'api_key_1', display_name: displayName, has_value: true, used_by: [],
    }));
    const dispatch = vi.fn();
    render(
      <TopBar
        client={fixtureClient({ renameApiKey })}
        dispatch={dispatch}
        onDeleteCharacter={vi.fn()}
        onDeleteForum={vi.fn()}
        onDeletePersona={vi.fn()}
        onCharacterDefinitionUpdated={vi.fn()}
        onForumDefinitionUpdated={vi.fn()}
        onPersonaDefinitionUpdated={vi.fn()}
        onProviderUpdated={vi.fn()}
        onStyleUpdated={vi.fn()}
        onVoiceUpdated={vi.fn()}
        state={{
          ...initialAppState,
          mainView: 'settings-api-key',
          inspectedApiKey: {
            id: 'api_key_1',
            name: 'Google',
          },
        }}
        title="Google"
      />,
    );

    await userEvent.click(screen.getByRole('button', { name: 'Rename Google' }));
    const name = screen.getByLabelText('API key name');
    await userEvent.clear(name);
    await userEvent.type(name, 'Gemini');
    await userEvent.click(screen.getByRole('button', { name: 'Save api key name' }));

    expect(renameApiKey).toHaveBeenCalledWith('api_key_1', 'Gemini');
    expect(dispatch).toHaveBeenCalledWith({
      type: 'api-key-updated', apiKeyId: 'api_key_1', apiKeyName: 'Gemini',
    });
  });
});
