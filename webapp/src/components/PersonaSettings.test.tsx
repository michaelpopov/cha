import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { initialAppState, type AppState } from '../state/view';
import {
  bootstrapFixture,
  fixtureClient,
  personaDetailFixture,
} from '../test/fixtures';
import { PersonaSettingsScreen } from './Screens';

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

function settingsState(): AppState {
  return {
    ...initialAppState,
    bootstrapStatus: 'ready',
    bootstrap: bootstrapFixture,
    mainView: 'persona-settings',
    inspectedPersona: {
      id: 'reader',
      writable: true,
    },
  };
}

function renderSettings(client = fixtureClient()) {
  const dispatch = vi.fn();
  render(
    <PersonaSettingsScreen
      client={client}
      dispatch={dispatch}
      state={settingsState()}
    />,
  );
  return dispatch;
}

describe('persona settings screen', () => {
  it('saves style without offering persona speech settings', async () => {
    const user = userEvent.setup();
    const updatePersona = vi.fn(async () => ({
      ...personaDetailFixture,
      style: 'mono-large',
    }));
    const dispatch = renderSettings(fixtureClient({ updatePersona }));

    expect(await screen.findByLabelText('Style')).toHaveValue('serif-italic');
    expect(screen.queryByLabelText('Voice')).not.toBeInTheDocument();
    expect(screen.queryByLabelText('Voice preview text')).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Play preview' })).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();

    await user.selectOptions(screen.getByLabelText('Style'), 'mono-large');
    await user.click(screen.getByRole('button', { name: 'Save' }));

    await waitFor(() => expect(updatePersona).toHaveBeenCalledWith('reader', {
      style: 'mono-large',
    }));
    expect(dispatch).toHaveBeenCalledWith({
      type: 'persona-updated',
      persona: expect.objectContaining({ id: 'reader', style: 'mono-large' }),
    });
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
  });

});
