import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { ChaError } from '../api/client';
import { initialAppState, type AppState } from '../state/view';
import {
  bootstrapFixture,
  characterDetailFixture,
  fixtureClient,
  voiceDetailFixture,
  voiceOutputRuntimeFixture,
} from '../test/fixtures';
import { CharacterSettingsScreen } from './Screens';

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

function settingsState(overrides: Partial<AppState> = {}): AppState {
  return {
    ...initialAppState,
    bootstrapStatus: 'ready',
    bootstrap: bootstrapFixture,
    mainView: 'character-settings',
    inspectedCharacter: {
      ...initialAppState.inspectedCharacter,
      id: 'guide',
      settingsWritable: true,
    },
    ...overrides,
  };
}

function renderSettings(client = fixtureClient()) {
  const dispatch = vi.fn();
  render(
    <CharacterSettingsScreen
      client={client}
      dispatch={dispatch}
      state={settingsState()}
    />,
  );
  return dispatch;
}

describe('character settings screen', () => {
  it('saves on-demand search overrides and restores the workspace default', async () => {
    const user = userEvent.setup();
    const updateCharacter = vi.fn(async (_id, update) => ({
      ...characterDetailFixture, ...update,
    }));
    renderSettings(fixtureClient({ updateCharacter }));
    const search = await screen.findByLabelText('On-demand web search');
    expect(search).toHaveValue('');
    for (const [value, expected] of [['true', true], ['false', false], ['', null]] as const) {
      await user.selectOptions(search, value);
      await user.click(screen.getByRole('button', { name: 'Save' }));
      expect(updateCharacter).toHaveBeenLastCalledWith('guide', expect.objectContaining({
        web_search_tool: expected,
      }));
      await waitFor(() => expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled());
    }
  });

  it('renders the pickers and saves character settings with none reasoning', async () => {
    const user = userEvent.setup();
    const updateCharacter = vi.fn(async () => ({
      ...characterDetailFixture,
      style: 'mono-large',
      voice_id: 'brian',
      reasoning_effort: 'none' as const,
      web_search: 'auto' as const,
    }));
    const dispatch = renderSettings(fixtureClient({ updateCharacter }));

    expect(await screen.findByLabelText('Provider')).toHaveValue('terra');
    expect(screen.getByRole('option', { name: 'Select provider' })).toBeDisabled();
    expect(screen.getByRole('option', { name: 'No style' })).toBeInTheDocument();
    const reasoning = screen.getByLabelText('Reasoning effort') as HTMLSelectElement;
    expect(reasoning).toHaveValue('');
    expect(Array.from(reasoning.options, (option) => option.value))
      .toEqual(['', 'none', 'low', 'medium', 'high', 'xhigh']);
    expect(screen.getByLabelText('Provider web search')).toHaveValue('');
    expect(screen.getByLabelText('Voice')).toHaveValue('');
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
    expect(screen.getByLabelText('Voice preview text')).toHaveClass(
      'cha-font-serif', 'cha-slant-italic',
    );

    await user.selectOptions(screen.getByLabelText('Style'), 'mono-large');
    await user.selectOptions(screen.getByLabelText('Reasoning effort'), 'none');
    await user.selectOptions(screen.getByLabelText('Provider web search'), 'auto');
    await user.selectOptions(screen.getByLabelText('Voice'), 'brian');
    expect(screen.getByLabelText('Voice preview text')).toHaveClass(
      'cha-font-mono', 'cha-scale-large',
    );
    expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
    expect(screen.queryByText(/restarts the sessions using this character/))
      .not.toBeInTheDocument();
    expect(screen.queryByText('Text to speak')).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Play preview' }))
      .not.toBeInTheDocument();

    await user.click(screen.getByRole('button', { name: 'Save' }));
    await waitFor(() => expect(updateCharacter).toHaveBeenCalledWith('guide', {
      provider: 'terra',
      style: 'mono-large',
      voice_id: 'brian',
      reasoning_effort: 'none' as const,
      web_search: 'auto',
      web_search_tool: null,
    }));
    expect(dispatch).toHaveBeenCalledWith({
      type: 'character-updated',
      character: expect.objectContaining({ id: 'guide', style: 'mono-large' }),
    });
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
  });

  it('tests the selected unsaved voice with editable text', async () => {
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
        pause: vi.fn(), removeAttribute: vi.fn(), load: vi.fn(),
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
    const preview = screen.getByRole('button', { name: 'Play preview' });
    expect(preview).toHaveClass('cha-button', 'cha-voice-preview-action');
    expect(preview).not.toHaveClass('cha-button-ghost');
    const text = screen.getByLabelText('Voice preview text');
    await userEvent.clear(text);
    await userEvent.type(text, 'Read this draft.');
    await userEvent.click(screen.getByRole('button', { name: 'Play preview' }));

    await waitFor(() => expect(previewSpeech).toHaveBeenCalledWith(
      'Read this draft.',
      voiceDetailFixture.elevenlabs_voice_id,
      { speed: 0.95 },
      expect.any(AbortSignal),
    ));
    expect(fetchMock).toHaveBeenCalledWith('/media/preview', expect.any(Object));
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
      voice_id: null,
      reasoning_effort: null,
      web_search: null, web_search_tool: null,
    }));
  });

  it('reports a failed save in place and keeps the edited values', async () => {
    const user = userEvent.setup();
    const updateCharacter = vi.fn(async () => {
      throw new ChaError('invalid_argument', 'Invalid character settings.');
    });
    renderSettings(fixtureClient({ updateCharacter }));

    await user.selectOptions(await screen.findByLabelText('Provider'), 'sol-high');
    await user.click(screen.getByRole('button', { name: 'Save' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('Invalid character settings.');
    expect(screen.getByLabelText('Provider')).toHaveValue('sol-high');
    expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
  });
});
