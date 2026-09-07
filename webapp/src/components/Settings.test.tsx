import { render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';

import type { ProviderDetail, StyleDetail } from '../api/client';
import { initialAppState } from '../state/view';
import { fixtureClient } from '../test/fixtures';
import {
  ApiKeyScreen,
  ApiKeysScreen,
  NewApiKeyScreen,
  NewProviderScreen,
  NewStyleScreen,
  ProviderScreen,
  SettingsNavigation,
  StyleScreen,
} from './Settings';
import { TopBar } from './TopBar';

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
  api_key_env: 'OPENROUTER_API_KEY',
  reasoning_effort: '',
  reasoning_format: 'auto',
  https: true,
  api: 'chat_completions',
  auth: 'none',
  web_search: 'off',
  cache_retention: 'short',
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

describe('Settings screens', () => {
  it('shows Providers, Styles, and API Keys as peer settings destinations', async () => {
    const dispatch = vi.fn();
    render(<SettingsNavigation dispatch={dispatch} />);

    expect(screen.getByRole('button', { name: /Providers/ })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: /Styles/ })).toBeInTheDocument();
    await userEvent.click(screen.getByRole('button', { name: /API Keys/ }));
    expect(dispatch).toHaveBeenCalledWith({ type: 'show-settings-api-keys' });
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

  it('stores a saved-key reference in a provider and clears its legacy env reference', async () => {
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
        state={{ ...initialAppState, inspectedProviderId: 'router' }}
      />,
    );
    await userEvent.selectOptions(await screen.findByLabelText('Credentials'), 'api_key_1');
    await userEvent.click(screen.getByRole('button', { name: 'Save changes' }));

    expect(updateProvider).toHaveBeenCalledWith(
      'router',
      expect.objectContaining({ api_key: 'api_key_1', api_key_env: null }),
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
    await userEvent.type(screen.getByLabelText('Name'), 'OpenRouter');
    await userEvent.click(screen.getByRole('button', { name: 'Create provider' }));

    expect(createProvider).toHaveBeenCalledWith({ display_name: 'OpenRouter' });
    expect(dispatch).toHaveBeenCalledWith({
      type: 'inspect-provider', providerId: 'provider_1', providerName: 'OpenRouter',
    });
  });

  it('shows only the essential provider settings', async () => {
    render(
      <ProviderScreen
        client={fixtureClient({
          getProvider: async () => provider,
          listApiKeys: async () => [{
            id: 'api_key_1', display_name: 'OpenRouter', has_value: true, used_by: [],
          }],
        })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={{ ...initialAppState, inspectedProviderId: provider.id }}
      />,
    );

    expect(await screen.findByLabelText('Model')).toHaveValue('openai/gpt-5');
    expect(screen.getByLabelText('Base URL')).toHaveValue('https://openrouter.ai/api');
    expect(screen.getByLabelText('API format')).toHaveValue('chat_completions');
    expect(screen.getByLabelText('Credentials')).toBeInTheDocument();
    expect(screen.queryByLabelText('Name')).not.toBeInTheDocument();
    expect(screen.queryByLabelText('Host')).not.toBeInTheDocument();
    expect(screen.queryByLabelText('Port')).not.toBeInTheDocument();
    expect(screen.queryByLabelText('Environment variable')).not.toBeInTheDocument();
    expect(screen.queryByLabelText('Reasoning effort')).not.toBeInTheDocument();
    expect(screen.getByLabelText('API format').parentElement?.parentElement)
      .toBe(screen.getByLabelText('Credentials').parentElement?.parentElement);
    expect(screen.getByText('Used by')).toBeInTheDocument();
    expect(screen.getByText('Guide')).toBeInTheDocument();
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
        state={{ ...initialAppState, inspectedProviderId: provider.id }}
      />,
    );

    await userEvent.type(await screen.findByLabelText('Model'), '-candidate');
    await userEvent.click(await screen.findByRole('button', { name: 'Test' }));

    expect(testProvider).toHaveBeenCalledWith(provider.id, expect.objectContaining({
      model: 'openai/gpt-5-candidate',
      api_key_env: null,
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
        state={{ ...initialAppState, inspectedProviderId: provider.id }}
      />,
    );

    expect(await screen.findByLabelText('Base URL'))
      .toHaveValue('http://127.0.0.1:11434/openai');
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
      api_key_env: null,
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
      api_key_env: null,
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
        state={{ ...initialAppState, inspectedProviderId: oauthProvider.id }}
      />,
    );

    expect(await screen.findByLabelText('Credentials')).toHaveDisplayValue('OpenAI OAuth');
    await userEvent.clear(screen.getByLabelText('Model'));
    await userEvent.type(screen.getByLabelText('Model'), 'gpt-5.6-sol');
    await userEvent.click(screen.getByRole('button', { name: 'Save changes' }));

    expect(updateProvider).toHaveBeenCalledWith(oauthProvider.id, expect.objectContaining({
      auth: 'openai_subscription',
      api_key: null,
      api_key_env: null,
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
        state={{ ...initialAppState, inspectedProviderId: provider.id }}
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
      inspectedProviderId: provider.id,
      inspectedProviderName: provider.display_name,
      providerEditingAvailable: true,
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
        state={{ ...initialAppState, inspectedStyleId: style.id }}
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
      inspectedStyleId: style.id,
      inspectedStyleName: style.display_name,
      styleEditingAvailable: true,
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
        state={{ ...initialAppState, inspectedProviderId: provider.id }}
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
        state={{ ...initialAppState, inspectedProviderId: provider.id }}
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
        state={{ ...initialAppState, inspectedStyleId: style.id }}
      />,
    );
    await userEvent.click(await screen.findByRole('button', { name: 'Delete style' }));
    const dialog = within(await screen.findByRole('dialog'));
    await userEvent.click(dialog.getByRole('button', { name: 'Delete style' }));

    expect(deleteStyle).toHaveBeenCalledWith(style.id);
    expect(dispatch).toHaveBeenCalledWith({ type: 'show-settings-styles' });
  });

  it('lists locally saved keys with provider usage', async () => {
    render(
      <ApiKeysScreen
        client={fixtureClient({ listApiKeys: async () => [{
          id: 'api_key_1', display_name: 'Google', has_value: true, used_by: ['Gemini'],
        }] })}
        dispatch={vi.fn()}
        sessionReport={null}
        state={initialAppState}
      />,
    );
    expect(await screen.findByRole('button', { name: /Google/ })).toHaveTextContent('Used by Gemini');
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
          inspectedApiKeyId: 'api_key_1',
          inspectedApiKeyName: 'Google',
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
          inspectedApiKeyId: 'api_key_1',
          inspectedApiKeyName: 'Google',
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
        state={{
          ...initialAppState,
          mainView: 'settings-api-key',
          inspectedApiKeyId: 'api_key_1',
          inspectedApiKeyName: 'Google',
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
