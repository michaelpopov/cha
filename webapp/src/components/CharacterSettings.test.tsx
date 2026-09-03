import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';

import { ChaError } from '../api/client';
import { initialAppState, type AppState } from '../state/view';
import {
  bootstrapFixture,
  characterDetailFixture,
  fixtureClient,
} from '../test/fixtures';
import { CharacterSettingsScreen } from './Screens';

function settingsState(overrides: Partial<AppState> = {}): AppState {
  return {
    ...initialAppState,
    bootstrapStatus: 'ready',
    bootstrap: bootstrapFixture,
    mainView: 'character-settings',
    inspectedCharacterId: 'guide',
    characterSettingsAvailable: true,
    ...overrides,
  };
}

function renderSettings(client = fixtureClient()) {
  const dispatch = vi.fn();
  render(
    <CharacterSettingsScreen
      client={client}
      dispatch={dispatch}
      sessionReport={null}
      state={settingsState()}
    />,
  );
  return dispatch;
}

describe('character settings screen', () => {
  it('renders the pickers, sample, and saves every character setting', async () => {
    const user = userEvent.setup();
    const updateCharacter = vi.fn(async () => ({
      ...characterDetailFixture,
      style: 'mono-large',
      reasoning_effort: 'high' as const,
      web_search: 'auto' as const,
    }));
    renderSettings(fixtureClient({ updateCharacter }));

    expect(await screen.findByLabelText('Provider')).toHaveValue('terra');
    expect(screen.getByRole('option', { name: 'Select provider' })).toBeDisabled();
    expect(screen.getByRole('option', { name: 'No style' })).toBeInTheDocument();
    expect(screen.getByLabelText('Reasoning effort')).toHaveValue('');
    expect(screen.getByLabelText('Web search')).toHaveValue('');
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
    expect(screen.getByText('The chief task in life is this…')).toHaveClass(
      'cha-font-serif', 'cha-slant-italic',
    );

    await user.selectOptions(screen.getByLabelText('Style'), 'mono-large');
    await user.selectOptions(screen.getByLabelText('Reasoning effort'), 'high');
    await user.selectOptions(screen.getByLabelText('Web search'), 'auto');
    expect(screen.getByText('The chief task in life is this…')).toHaveClass(
      'cha-font-mono', 'cha-scale-large',
    );
    expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
    expect(screen.getByText(/restarts the sessions using this character/)).toBeInTheDocument();

    await user.click(screen.getByRole('button', { name: 'Save' }));
    await waitFor(() => expect(updateCharacter).toHaveBeenCalledWith('guide', {
      provider: 'terra',
      style: 'mono-large',
      reasoning_effort: 'high',
      web_search: 'auto',
    }));
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
  });

  it('shows a saved provider the workspace can no longer resolve', async () => {
    // The name is absent from available_providers because loading it failed,
    // but it is still what the file says and still what a save resubmits.
    renderSettings(fixtureClient({
      getCharacter: async () => ({ ...characterDetailFixture, provider: 'gone' }),
    }));

    const provider = await screen.findByLabelText('Provider') as HTMLSelectElement;
    expect(provider.value).toBe('gone');
    expect(provider.options[provider.selectedIndex].textContent).toBe('gone (not available)');
  });

  it('keeps save disabled until a character without a provider has one', async () => {
    // A character file may omit 'provider', and a save without one is
    // rejected, so editing only the style must not offer a save that silently
    // does nothing.
    const user = userEvent.setup();
    const updateCharacter = vi.fn(async () => characterDetailFixture);
    renderSettings(fixtureClient({
      getCharacter: async () => ({ ...characterDetailFixture, provider: null }),
      updateCharacter,
    }));

    expect(await screen.findByLabelText('Provider')).toHaveValue('');
    await user.selectOptions(screen.getByLabelText('Style'), 'mono-large');
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();

    await user.selectOptions(screen.getByLabelText('Provider'), 'terra');
    expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
    await user.click(screen.getByRole('button', { name: 'Save' }));
    await waitFor(() => expect(updateCharacter).toHaveBeenCalledWith('guide', {
      provider: 'terra',
      style: 'mono-large',
      reasoning_effort: null,
      web_search: null,
    }));
  });

  it('reports a failed save in place and keeps the edited values', async () => {
    const user = userEvent.setup();
    const updateCharacter = vi.fn(async () => {
      throw new ChaError(400, 'bad_request', 'Invalid character settings.');
    });
    renderSettings(fixtureClient({ updateCharacter }));

    await user.selectOptions(await screen.findByLabelText('Provider'), 'sol-high');
    await user.click(screen.getByRole('button', { name: 'Save' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('Invalid character settings.');
    expect(screen.getByLabelText('Provider')).toHaveValue('sol-high');
    expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
  });
});
