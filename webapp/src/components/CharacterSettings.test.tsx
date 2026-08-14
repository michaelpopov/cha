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
  it('renders the pickers, sample, and a save that writes both values', async () => {
    const user = userEvent.setup();
    const updateCharacter = vi.fn(async () => ({
      ...characterDetailFixture,
      style: 'mono-large',
    }));
    renderSettings(fixtureClient({ updateCharacter }));

    expect(await screen.findByLabelText('Provider')).toHaveValue('terra');
    expect(screen.getByRole('option', { name: 'Workspace default' })).toBeInTheDocument();
    expect(screen.getByRole('option', { name: 'No style' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
    expect(screen.getByText('The chief task in life is this…')).toHaveClass(
      'cha-font-serif', 'cha-slant-italic',
    );

    await user.selectOptions(screen.getByLabelText('Style'), 'mono-large');
    expect(screen.getByText('The chief task in life is this…')).toHaveClass(
      'cha-font-mono', 'cha-scale-large',
    );
    expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
    expect(screen.getByText(/restarts the sessions using this character/)).toBeInTheDocument();

    await user.click(screen.getByRole('button', { name: 'Save' }));
    await waitFor(() => expect(updateCharacter).toHaveBeenCalledWith('guide', {
      provider: 'terra',
      style: 'mono-large',
    }));
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
  });

  it('names an override that makes the provider picker currently inert', async () => {
    const bootstrap = structuredClone(bootstrapFixture);
    bootstrap.forums[1] = { ...bootstrap.forums[1], display_name: 'Circle of Life' };
    const dispatch = vi.fn();
    render(
      <CharacterSettingsScreen
        client={fixtureClient({
          getCharacter: async () => ({
            ...characterDetailFixture,
            provider_overridden_by: ['Circle of Life'],
          }),
        })}
        dispatch={dispatch}
        sessionReport={null}
        state={settingsState({ bootstrap })}
      />,
    );

    expect(await screen.findByText(
      'Circle of Life uses its own provider for this character, and it is the only forum Guide belongs to.',
    )).toBeInTheDocument();
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

  it('reports a failed save in place and keeps the edited values', async () => {
    const user = userEvent.setup();
    const updateCharacter = vi.fn(async () => {
      throw new ChaError(400, 'bad_request', 'Invalid provider or style.');
    });
    renderSettings(fixtureClient({ updateCharacter }));

    await user.selectOptions(await screen.findByLabelText('Provider'), '');
    await user.click(screen.getByRole('button', { name: 'Save' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('Invalid provider or style.');
    expect(screen.getByLabelText('Provider')).toHaveValue('');
    expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
  });
});
