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
      type: 'inspect-api-key', apiKeyId: 'api_key_1',
    });
    expect(screen.getByLabelText('API key')).toHaveValue('');
  });

  it('stores a saved-key reference in a provider and clears its legacy env reference', async () => {
    const updateProvider = vi.fn(async (_id, update) => ({
      id: 'router', writable: true, ...update,
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
    await userEvent.selectOptions(await screen.findByLabelText('Saved API key'), 'api_key_1');
    await userEvent.click(screen.getByRole('button', { name: 'Save provider' }));

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
      type: 'inspect-provider', providerId: 'provider_1',
    });
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
      type: 'inspect-style', styleId: 'style_1',
    });
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
        state={{ ...initialAppState, inspectedApiKeyId: 'api_key_1' }}
      />,
    );
    await userEvent.click(await screen.findByRole('button', { name: 'Remove API key' }));
    const dialog = within(await screen.findByRole('dialog'));
    await userEvent.click(dialog.getByRole('button', { name: 'Remove API key' }));

    expect(deleteApiKey).toHaveBeenCalledWith('api_key_1');
    expect(dispatch).toHaveBeenCalledWith({ type: 'show-settings-api-keys' });
  });
});
