import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { initialAppState, type AppState } from '../state/view';
import {
  bootstrapFixture,
  fixtureClient,
  personaDetailFixture,
  voiceDetailFixture,
  voiceOutputRuntimeFixture,
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
      sessionReport={null}
      state={settingsState()}
    />,
  );
  return dispatch;
}

describe('persona settings screen', () => {
  it('renders style and voice pickers and saves both settings', async () => {
    const user = userEvent.setup();
    const updatePersona = vi.fn(async () => ({
      ...personaDetailFixture,
      style: 'mono-large',
      voice_id: 'brian',
    }));
    const dispatch = renderSettings(fixtureClient({ updatePersona }));

    expect(await screen.findByLabelText('Style')).toHaveValue('serif-italic');
    expect(screen.getByLabelText('Voice')).toHaveValue('');
    expect(screen.getByLabelText('Voice preview text')).toHaveClass(
      'cha-font-serif', 'cha-slant-italic',
    );
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();

    await user.selectOptions(screen.getByLabelText('Style'), 'mono-large');
    await user.selectOptions(screen.getByLabelText('Voice'), 'brian');
    expect(screen.getByLabelText('Voice preview text')).toHaveClass(
      'cha-font-mono', 'cha-scale-large',
    );
    await user.click(screen.getByRole('button', { name: 'Save' }));

    await waitFor(() => expect(updatePersona).toHaveBeenCalledWith('reader', {
      style: 'mono-large',
      voice_id: 'brian',
    }));
    expect(dispatch).toHaveBeenCalledWith({
      type: 'persona-updated',
      persona: expect.objectContaining({ id: 'reader', style: 'mono-large' }),
    });
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
  });

  it('tests the selected voice with editable preview text', async () => {
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
    renderSettings(fixtureClient({
      previewSpeech,
      listVoices: async () => [voiceDetailFixture],
      getVoiceOutputRuntime: async () => ({
        ...voiceOutputRuntimeFixture,
        url: 'https://api.fish.audio/v1/tts',
        model: 's2.1-pro',
        output_format: 'mp3',
      }),
    }));

    await userEvent.selectOptions(await screen.findByLabelText('Voice'), 'brian');
    const text = screen.getByLabelText('Voice preview text');
    await userEvent.clear(text);
    await userEvent.type(text, 'Read this persona draft.');
    await userEvent.click(screen.getByRole('button', { name: 'Play preview' }));

    await waitFor(() => expect(previewSpeech).toHaveBeenCalledWith(
      'Read this persona draft.',
      voiceDetailFixture.elevenlabs_voice_id,
      { speed: 0.95 },
      expect.any(AbortSignal),
    ));
    expect(fetchMock).toHaveBeenCalledWith('/media/preview', expect.any(Object));
  });
});
