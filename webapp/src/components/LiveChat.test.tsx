import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { ChaError, type ChaClient, type SessionSnapshot } from '../api/client';
import type { SessionEventHandlers } from '../api/events';
import { bootstrapFixture, fixtureClient, plainVoice, snapshotFixture } from '../test/fixtures';
import { TextToSpeechSession } from '../textToSpeech';
import { VoiceInputSession } from '../voiceInput';
import { App } from './App';
import { formatEntryTime } from './ChatScreen';

afterEach(() => {
  delete window.chaVoiceInput;
  delete window.chaTextToSpeech;
  vi.restoreAllMocks();
});

function drivableEvents() {
  const handlers: SessionEventHandlers[] = [];
  const connections: { key: string; close: ReturnType<typeof vi.fn> }[] = [];
  return {
    handlers,
    connections,
    connect(forumId: string, sessionId: string, given: SessionEventHandlers) {
      handlers.push(given);
      const connection = { key: `${forumId}/${sessionId}`, close: vi.fn() };
      connections.push(connection);
      return connection;
    },
  };
}

async function attachInitial(
  events: ReturnType<typeof drivableEvents>,
  snapshot: SessionSnapshot = snapshotFixture,
) {
  await waitFor(() => expect(events.connections[0]?.key).toBe('entrance/welcome'));
  act(() => events.handlers[0].onSnapshot(snapshot));
  await waitFor(() => expect(screen.getByRole('textbox', { name: 'Message' })).toBeEnabled());
}

function transcriptSnapshot(): SessionSnapshot {
  return {
    ...snapshotFixture,
    transcript: [{
      id: 4,
      kind: 'character',
      participant_id: 'assistant',
      display_name: 'Assistant',
      addressed_to: 'guest',
      addressed_to_name: 'Guest',
      text: 'Still here',
      status: 'streaming',
      request_id: 7,
      created_at: null,
    }],
    generation: {
      active: true,
      request_id: 7,
      character_id: 'assistant',
      character_display_name: 'Assistant',
      phase: 'reasoning',
      reasoning_text: 'Checking',
    },
  };
}

// jsdom has no layout, so the three numbers the transcript reads to decide
// whether it is still following the newest text have to be supplied directly.
function placeReadingPosition(
  element: HTMLElement,
  position: { scrollTop: number; scrollHeight: number; clientHeight: number },
) {
  for (const [name, value] of Object.entries(position)) {
    Object.defineProperty(element, name, { configurable: true, value });
  }
  fireEvent.scroll(element);
}

describe('live chat', () => {
  it('opens and attaches the initial conversation on plain startup and Return to Welcome', async () => {
    const events = drivableEvents();
    const openSession = vi.fn(async (forumId: string, sessionId: string) => ({
      forum_id: forumId,
      session_id: sessionId,
    }));
    const client = fixtureClient({
      openSession,
      listSessions: async () => [{
        id: 'planning', label: 'Planning', live: false, updated_at: 1,
      }],
      getSessionSnapshot: async (forumId) => forumId === 'lobby'
        ? {
            ...snapshotFixture,
            forum: bootstrapFixture.forums[1],
            session_id: 'planning',
            session_label: 'Planning',
            characters: [bootstrapFixture.characters[1]],
            default_character_id: 'guide',
          }
        : snapshotFixture,
    });
    render(<App client={client} connectSessionEvents={events.connect} />);
    await attachInitial(events);
    expect(openSession).toHaveBeenCalledWith('entrance', 'welcome');

    fireEvent.click(screen.getByRole('button', { name: 'Forums' }));
    fireEvent.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
    fireEvent.click(await screen.findByRole('button', { name: /^Planning/ }));
    await waitFor(() => expect(events.connections.some(({ key }) => key === 'lobby/planning')).toBe(true));

    window.history.replaceState(null, '', '/');
    window.dispatchEvent(new PopStateEvent('popstate'));
    await waitFor(() => expect(
      events.connections.filter(({ key }) => key === 'entrance/welcome'),
    ).toHaveLength(2));
    expect(openSession.mock.calls.filter(([, id]) => id === 'welcome')).toHaveLength(2);
  });

  it('renders snapshot state and applies entry and reasoning append events', async () => {
    const events = drivableEvents();
    const snapshot = transcriptSnapshot();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, snapshot);

    expect(screen.getByText('Still here')).toBeInTheDocument();
    expect(screen.getByText('Checking')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Stop generation' })).toBeEnabled();

    act(() => events.handlers[0].onAppend({
      target: { kind: 'entry', entry_id: 4 }, text: ' with you', seq: 0,
    }));
    act(() => events.handlers[0].onAppend({
      target: { kind: 'reasoning', request_id: 7 }, text: ' again', seq: 1,
    }));
    expect(screen.getByText('Still here with you')).toBeInTheDocument();
    expect(screen.getByText('Checking again')).toBeInTheDocument();
  });

  it('shows the creation time under timestamped entries and nothing for unknown times', async () => {
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: [
        {
          id: 1, kind: 'human', participant_id: 'guest', display_name: 'Guest',
          addressed_to: 'assistant', addressed_to_name: 'Assistant',
          text: 'Question', status: 'complete', created_at: 1_700_000_000,
        },
        {
          id: 2, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
          addressed_to: '', addressed_to_name: '',
          text: 'Old answer', status: 'complete', created_at: null,
        },
      ],
    });

    const articles = document.querySelectorAll('.cha-message');
    expect(articles).toHaveLength(2);
    const stamped = articles[0].querySelector('.cha-message-time');
    expect(stamped).not.toBeNull();
    expect(stamped?.getAttribute('dateTime'))
      .toBe(new Date(1_700_000_000 * 1000).toISOString());
    expect(stamped?.textContent).toBe(formatEntryTime(1_700_000_000));
    expect(articles[1].querySelector('.cha-message-time')).toBeNull();
  });

  it('offers text to speech only for completed model responses in the macOS shell', async () => {
    const play = vi.spyOn(TextToSpeechSession.prototype, 'play').mockResolvedValue();
    const stop = vi.spyOn(TextToSpeechSession.prototype, 'stop');
    window.chaTextToSpeech = {
      url: 'https://api.elevenlabs.io/v1/text-to-speech/voice',
      apiKey: 'secret',
      model: 'eleven_multilingual_v2',
    };
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: [
        {
          id: 1, kind: 'human', participant_id: 'guest', display_name: 'Guest',
          addressed_to: 'assistant', addressed_to_name: 'Assistant',
          text: 'Question', status: 'complete', created_at: 1_700_000_000,
        },
        {
          id: 2, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
          addressed_to: '', addressed_to_name: '',
          text: 'Answer', status: 'complete', created_at: 1_700_000_001,
        },
      ],
    });

    expect(screen.queryByRole('button', { name: "Read Guest's response aloud" }))
      .not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: "Read Assistant's response aloud" }));
    expect(play).toHaveBeenCalledOnce();
    fireEvent.click(await screen.findByRole('button', {
      name: "Stop reading Assistant's response",
    }));
    expect(stop).toHaveBeenCalledOnce();
  });

  it('covers through a cancelled response and shows one uncover control', async () => {
    const coverConversation = vi.fn(async () => ({ clear_input: false }));
    const uncoverConversation = vi.fn(async () => ({ clear_input: false }));
    const events = drivableEvents();
    const transcript: SessionSnapshot['transcript'] = [
      {
        id: 1, kind: 'human', participant_id: 'guest', display_name: 'Guest',
        addressed_to: 'assistant', addressed_to_name: 'Assistant',
        text: 'Question', status: 'complete', created_at: 1_700_000_000,
      },
      {
        id: 2, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
        addressed_to: '', addressed_to_name: '',
        text: 'Answer', status: 'complete', created_at: 1_700_000_001,
      },
      {
        id: 3, kind: 'human', participant_id: 'guest', display_name: 'Guest',
        addressed_to: 'guide', addressed_to_name: 'Guide',
        text: 'Follow-up', status: 'complete', created_at: 1_700_000_002,
      },
      {
        id: 4, kind: 'character', participant_id: 'guide', display_name: 'Guide',
        addressed_to: '', addressed_to_name: '',
        text: 'More detail', status: 'complete', created_at: 1_700_000_003,
      },
      {
        id: 5, kind: 'human', participant_id: 'guest', display_name: 'Guest',
        addressed_to: 'critic', addressed_to_name: 'Critic',
        text: 'Last question', status: 'complete', created_at: 1_700_000_004,
      },
      {
        id: 6, kind: 'character', participant_id: 'critic', display_name: 'Critic',
        addressed_to: '', addressed_to_name: '',
        text: 'Partial answer', status: 'cancelled', created_at: 1_700_000_005,
      },
    ];
    render(<App
      client={fixtureClient({ coverConversation, uncoverConversation })}
      connectSessionEvents={events.connect}
    />);
    await attachInitial(events, { ...snapshotFixture, transcript });

    fireEvent.click(screen.getByRole('button', {
      name: "Cover transcript through Critic's response",
    }));
    await waitFor(() => expect(coverConversation).toHaveBeenCalledWith(
      'entrance', 'welcome', { through_entry_id: 6 },
    ));

    act(() => events.handlers[0].onSnapshot({
      ...snapshotFixture,
      transcript: [...transcript, {
        id: 7, kind: 'notice', participant_id: '', display_name: 'cover',
        addressed_to: '', addressed_to_name: '', text: '', status: 'complete',
        created_at: null,
      }],
      covered_until: 7,
    }));
    expect(screen.getAllByRole('button', { name: 'Uncover transcript' })).toHaveLength(1);
    fireEvent.click(screen.getByRole('button', { name: 'Uncover transcript' }));
    await waitFor(() => expect(uncoverConversation).toHaveBeenCalledWith(
      'entrance', 'welcome',
    ));
  });

  it('shades the active covered prefix and removes the shading after uncover', async () => {
    const events = drivableEvents();
    const human = (id: number, text: string): SessionSnapshot['transcript'][number] => ({
      id, kind: 'human', participant_id: 'guest', display_name: 'Guest',
      addressed_to: 'assistant', addressed_to_name: 'Assistant',
      text, status: 'complete', created_at: null,
    });
    const character = (id: number, text: string): SessionSnapshot['transcript'][number] => ({
      id, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
      addressed_to: '', addressed_to_name: '',
      text, status: 'complete', created_at: null,
    });
    const marker = (
      id: number,
      name: 'cover' | 'uncover',
    ): SessionSnapshot['transcript'][number] => ({
      id, kind: 'notice', participant_id: '', display_name: name,
      addressed_to: '', addressed_to_name: '',
      text: '', status: 'complete', created_at: null,
    });
    const firstCover = [
      human(1, 'Earlier question'),
      character(2, 'Earlier answer'),
      marker(3, 'cover'),
      human(4, 'After cover'),
    ];

    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: firstCover,
      covered_until: 3,
    });

    let covered = screen.getByRole('region', { name: 'Covered conversation' });
    expect(within(covered).getByText('Earlier question')).toBeInTheDocument();
    expect(within(covered).getByText('Earlier answer')).toBeInTheDocument();
    expect(within(covered).queryByText('After cover')).not.toBeInTheDocument();
    expect(document.querySelectorAll('.cha-message.is-notice')).toHaveLength(0);

    const secondCover = [...firstCover, marker(5, 'cover'), human(6, 'After second cover')];
    act(() => events.handlers[0].onSnapshot({
      ...snapshotFixture,
      transcript: secondCover,
      covered_until: 5,
    }));

    covered = screen.getByRole('region', { name: 'Covered conversation' });
    expect(within(covered).getByText('After cover')).toBeInTheDocument();
    expect(within(covered).queryByText('After second cover')).not.toBeInTheDocument();
    expect(document.querySelectorAll('.cha-message.is-notice')).toHaveLength(0);

    act(() => events.handlers[0].onSnapshot({
      ...snapshotFixture,
      transcript: [...secondCover, marker(7, 'uncover')],
    }));

    expect(screen.queryByRole('region', { name: 'Covered conversation' }))
      .not.toBeInTheDocument();
    expect(screen.getByText('Earlier question')).toBeInTheDocument();
    expect(document.querySelectorAll('.cha-message.is-notice')).toHaveLength(0);
  });

  it('shows a multicast prompt once while keeping every character response', async () => {
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: [
        {
          id: 1, kind: 'human', participant_id: 'guest', display_name: 'Guest',
          addressed_to: 'one', addressed_to_name: 'One',
          text: 'Shared question', status: 'complete', request_id: 10, created_at: 100,
        },
        {
          id: 2, kind: 'character', participant_id: 'one', display_name: 'One',
          addressed_to: '', addressed_to_name: '',
          text: 'One answer', status: 'complete', request_id: 10, created_at: 101,
        },
        {
          id: 3, kind: 'human', participant_id: 'guest', display_name: 'Guest',
          addressed_to: 'two', addressed_to_name: 'Two',
          text: 'Shared question', status: 'complete', request_id: 11, created_at: 100,
        },
        {
          id: 4, kind: 'character', participant_id: 'two', display_name: 'Two',
          addressed_to: '', addressed_to_name: '',
          text: 'Two answer', status: 'complete', request_id: 11, created_at: 102,
        },
      ],
    });

    expect(screen.getAllByText('Shared question')).toHaveLength(1);
    expect(screen.getByText('One answer')).toBeInTheDocument();
    expect(screen.getByText('Two answer')).toBeInTheDocument();
    expect(document.querySelectorAll('.cha-message')).toHaveLength(3);
    expect(document.querySelectorAll('.cha-repeated-prompt-divider')).toHaveLength(1);
  });

  it('hides an indented echoed UTC metadata line from a character response', async () => {
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: [{
        id: 1, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
        addressed_to: '', addressed_to_name: '',
        text: '  [2026-08-18T22:11:46.123Z]\r\n\r\nHello anyway!',
        status: 'complete', created_at: 1_787_120_306,
      }],
    });

    const response = document.querySelector('.cha-message-text');
    expect(response).toHaveTextContent('Hello anyway!');
    expect(response).not.toHaveTextContent('[2026-08-18T22:11:46.123Z]');
  });

  it('submits with the forum persona, clears accepted input, and preserves a failed draft', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    const submitInput = vi.fn()
      .mockRejectedValueOnce(new ChaError(400, 'bad_request', 'The prompt was not accepted.'))
      .mockResolvedValueOnce({ clear_input: true });
    render(
      <App
        client={fixtureClient({ submitInput })}
        connectSessionEvents={events.connect}
      />,
    );
    await attachInitial(events);

    const input = screen.getByRole('textbox', { name: 'Message' });
    await user.type(input, 'Keep this draft');
    await user.click(screen.getByRole('button', { name: 'Send message' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('not accepted');
    expect(input).toHaveValue('Keep this draft');
    expect(submitInput).toHaveBeenLastCalledWith(
      'entrance', 'welcome', { text: 'Keep this draft' },
    );

    await user.click(screen.getByRole('button', { name: 'Send message' }));
    await waitFor(() => expect(input).toHaveValue(''));
  });

  it('sends to all characters without changing the session default', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    const submitInput = vi.fn()
      .mockRejectedValueOnce(new ChaError(400, 'bad_request', 'The prompt was not accepted.'))
      .mockResolvedValue({ clear_input: true });
    const setDefaultCharacter = vi.fn(async () => ({ clear_input: false }));
    const snapshot: SessionSnapshot = {
      ...snapshotFixture,
      forum: {
        ...snapshotFixture.forum,
        members: bootstrapFixture.characters,
      },
      characters: bootstrapFixture.characters,
    };
    const nextSnapshot: SessionSnapshot = {
      ...snapshotFixture,
      forum: bootstrapFixture.forums[1],
      session_id: 'planning',
      session_label: 'Planning',
      characters: [bootstrapFixture.characters[1]],
      default_character_id: 'guide',
    };
    render(
      <App
        client={fixtureClient({
          getSessionSnapshot: async (forumId) => (
            forumId === 'lobby' ? nextSnapshot : snapshot
          ),
          listSessions: async () => [{
            id: 'planning', label: 'Planning', live: false, updated_at: 1,
          }],
          setDefaultCharacter,
          submitInput,
        })}
        connectSessionEvents={events.connect}
      />,
    );
    await attachInitial(events, snapshot);

    const chooser = screen.getByRole('combobox', { name: 'Choose message target' });
    const input = screen.getByRole('textbox', { name: 'Message' });
    expect(within(chooser).getByRole('option', { name: 'All characters' }))
      .toBeInTheDocument();

    await user.selectOptions(chooser, '*');
    expect(setDefaultCharacter).not.toHaveBeenCalled();
    expect(chooser).toHaveValue('*');
    expect(input).toHaveAttribute('placeholder', 'Message all characters');
    expect(screen.getByLabelText('Current chat context')).toHaveTextContent('To: All characters');

    await user.type(input, 'Shared question');
    await user.click(screen.getByRole('button', { name: 'Send message' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('not accepted');
    expect(submitInput).toHaveBeenLastCalledWith(
      'entrance', 'welcome', { text: '/mcast Shared question' },
    );
    expect(input).toHaveValue('Shared question');
    expect(chooser).toHaveValue('*');

    await user.click(screen.getByRole('button', { name: 'Send message' }));
    await waitFor(() => expect(input).toHaveValue(''));
    expect(chooser).toHaveValue('*');

    await user.type(input, '@Guide is part of the question');
    await user.click(screen.getByRole('button', { name: 'Send message' }));
    await waitFor(() => expect(input).toHaveValue(''));
    expect(submitInput).toHaveBeenLastCalledWith(
      'entrance', 'welcome', { text: '/mcast @@Guide is part of the question' },
    );

    await user.click(screen.getByRole('button', { name: 'Forums' }));
    await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
    const sessions = await screen.findByRole('region', { name: 'Forum sessions navigation' });
    await user.click(within(sessions).getByRole('button', { name: /^Planning/ }));
    const nextChooser = await screen.findByRole('combobox', { name: 'Choose message target' });
    const nextInput = screen.getByRole('textbox', { name: 'Message' });
    await waitFor(() => expect(nextChooser).toHaveValue('guide'));
    expect(nextInput).toHaveAttribute('placeholder', 'Message Guide');
    expect(screen.getByLabelText('Current chat context')).toHaveTextContent('To: Guide');
  });

  it('appends realtime voice deltas and waits for the final transcript before sending', async () => {
    window.chaVoiceInput = {
      url: 'https://api.openai.com/v1/realtime/calls',
      apiKey: 'secret',
      model: 'gpt-live-transcribe',
    };
    vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
    let appendVoice = (_text: string) => {};
    let finishRecording = () => {};
    const stopped = new Promise<void>((resolve) => { finishRecording = resolve; });
    const voiceSession = {
      stop: vi.fn(() => stopped),
      cancel: vi.fn(),
    } as unknown as VoiceInputSession;
    const startVoiceInput = vi.spyOn(VoiceInputSession, 'start').mockImplementation(
      async (_configuration, onTranscription) => {
        appendVoice = onTranscription;
        return voiceSession;
      },
    );
    const events = drivableEvents();
    const submitInput = vi.fn(async () => ({ clear_input: true }));
    render(
      <App
        client={fixtureClient({ submitInput })}
        connectSessionEvents={events.connect}
      />,
    );
    await attachInitial(events);

    const input = screen.getByRole('textbox', { name: 'Message' });
    fireEvent.click(screen.getByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    expect(startVoiceInput.mock.calls[0]?.[0].languages).toEqual(['en']);
    fireEvent.change(input, { target: { value: 'Typed' } });
    act(() => {
      appendVoice('spoken');
      appendVoice(' words.');
    });
    expect(input).toHaveValue('Typed spoken words.');

    fireEvent.click(screen.getByRole('button', { name: 'Send message' }));
    expect(voiceSession.stop).toHaveBeenCalledOnce();
    expect(submitInput).not.toHaveBeenCalled();
    act(() => appendVoice(' Final words.'));
    await act(async () => finishRecording());
    await waitFor(() => expect(submitInput).toHaveBeenCalledWith(
      'entrance', 'welcome', { text: 'Typed spoken words. Final words.' },
    ));

  });

  it('uses Russian voice input when composer transliteration is enabled', async () => {
    window.chaVoiceInput = {
      url: 'https://api.openai.com/v1/realtime/calls',
      apiKey: 'secret',
      model: 'gpt-live-transcribe',
    };
    vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
    const voiceSession = {
      stop: vi.fn(async () => {}),
      cancel: vi.fn(),
    } as unknown as VoiceInputSession;
    const startVoiceInput = vi.spyOn(VoiceInputSession, 'start')
      .mockResolvedValue(voiceSession);
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events);

    const toggle = screen.getByRole('button', { name: 'Latin to Russian transliteration' });
    await userEvent.setup().click(toggle);
    fireEvent.click(screen.getByRole('button', { name: 'Start voice input' }));

    await waitFor(() => expect(startVoiceInput).toHaveBeenCalledOnce());
    expect(startVoiceInput.mock.calls[0]?.[0].languages).toEqual(['ru']);
  });

  it('sends a draft with Enter and adds a line with Ctrl+Enter', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    const submitInput = vi.fn(async () => ({ clear_input: true }));
    render(
      <App
        client={fixtureClient({ submitInput })}
        connectSessionEvents={events.connect}
      />,
    );
    await attachInitial(events);

    const input = screen.getByRole('textbox', { name: 'Message' });
    expect(input.tagName).toBe('TEXTAREA');
    expect(input).toHaveAttribute('rows', '1');
    Object.defineProperty(input, 'scrollHeight', { configurable: true, value: 72 });

    await user.type(input, 'First line{Control>}{Enter}{/Control}Second line');

    expect(input).toHaveValue('First line\nSecond line');
    expect(input).toHaveStyle({ height: '72px' });
    expect(submitInput).not.toHaveBeenCalled();

    await user.type(input, '{Enter}');
    await waitFor(() => expect(submitInput).toHaveBeenCalledWith(
      'entrance', 'welcome', { text: 'First line\nSecond line' },
    ));
  });

  it('transliterates Latin typing to Russian when the composer mode is enabled', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events);

    const input = screen.getByRole('textbox', { name: 'Message' });
    const toggle = screen.getByRole('button', { name: 'Latin to Russian transliteration' });
    expect(toggle).toHaveAttribute('aria-pressed', 'false');

    await user.click(toggle);
    expect(toggle).toHaveAttribute('aria-pressed', 'true');
    await user.type(input, "Privet, shhuka mozhet s+hodit' v raj+on.");
    expect(input).toHaveValue('Привет, щука может сходить в район.');
  });

  it('resizes the message editor in both directions and clamps its height', async () => {
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events);

    const chat = screen.getByLabelText('Chat area');
    const input = screen.getByRole('textbox', { name: 'Message' });
    const divider = screen.getByRole('separator', { name: 'Resize message editor' });
    Object.defineProperty(chat, 'clientHeight', { configurable: true, value: 500 });
    Object.defineProperty(input, 'offsetHeight', {
      configurable: true,
      get: () => Number.parseFloat(input.style.height) || 40,
    });
    Object.defineProperty(input, 'scrollHeight', {
      configurable: true,
      get: () => input.style.height === 'auto'
        ? 30
        : Math.max(30, Number.parseFloat(input.style.height) || 0),
    });

    fireEvent.pointerDown(divider, { clientY: 300, pointerId: 1 });
    fireEvent.pointerMove(divider, { clientY: -100, pointerId: 1 });
    expect(input).toHaveStyle({ height: '400px' });
    fireEvent.pointerUp(divider, { pointerId: 1 });

    fireEvent.pointerDown(divider, { clientY: 100, pointerId: 2 });
    fireEvent.pointerMove(divider, { clientY: 350, pointerId: 2 });
    expect(input).toHaveStyle({ height: '150px' });
    fireEvent.pointerMove(divider, { clientY: 600, pointerId: 2 });
    expect(input).toHaveStyle({ height: '30px' });
    fireEvent.pointerUp(divider, { pointerId: 2 });
  });

  it('keeps a draft typed while the previous send was still in flight', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    let accept: (result: { clear_input: boolean }) => void = () => {};
    const submitInput = vi.fn(() => new Promise<{ clear_input: boolean }>((resolve) => {
      accept = resolve;
    }));
    render(
      <App
        client={fixtureClient({ submitInput })}
        connectSessionEvents={events.connect}
      />,
    );
    await attachInitial(events);

    const input = screen.getByRole('textbox', { name: 'Message' });
    await user.type(input, 'First message');
    await user.click(screen.getByRole('button', { name: 'Send message' }));
    await user.clear(input);
    await user.type(input, 'Second message');
    await act(async () => accept({ clear_input: true }));

    expect(input).toHaveValue('Second message');
  });

  it('opens a new conversation with an empty composer', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    const planning: SessionSnapshot = {
      ...snapshotFixture,
      forum: bootstrapFixture.forums[1],
      session_id: 'planning',
      session_label: 'Planning',
      characters: [bootstrapFixture.characters[1]],
      default_character_id: 'guide',
    };
    const client = fixtureClient({
      getSessionSnapshot: async (forumId) => forumId === 'lobby' ? planning : snapshotFixture,
    });
    render(<App client={client} connectSessionEvents={events.connect} />);
    await attachInitial(events);

    await user.type(screen.getByRole('textbox', { name: 'Message' }), 'Only for Welcome');
    const recent = within(screen.getByLabelText('Recent sessions'));
    await user.click(recent.getByRole('button', { name: /^Planning/ }));
    await waitFor(() => expect(events.connections[1]?.key).toBe('lobby/planning'));
    act(() => events.handlers[1].onSnapshot(planning));

    const input = await screen.findByRole('textbox', { name: 'Message' });
    await waitFor(() => expect(input).toBeEnabled());
    expect(input).toHaveValue('');
  });

  // The server holds a mutation until its command deadline, so a request left
  // behind in one conversation can outlive the reader's presence in it.
  it('sends in one conversation while another still has a request in flight', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    const planning: SessionSnapshot = {
      ...snapshotFixture,
      forum: bootstrapFixture.forums[1],
      session_id: 'planning',
      session_label: 'Planning',
      characters: [bootstrapFixture.characters[1]],
      default_character_id: 'guide',
    };
    const submitInput = vi.fn((_forumId: string, sessionId: string) => (
      sessionId === 'welcome'
        ? new Promise<{ clear_input: boolean }>(() => {})
        : Promise.resolve({ clear_input: true })
    ));
    const client = fixtureClient({
      submitInput,
      getSessionSnapshot: async (forumId) => forumId === 'lobby' ? planning : snapshotFixture,
    });
    render(<App client={client} connectSessionEvents={events.connect} />);
    await attachInitial(events);

    await user.type(screen.getByRole('textbox', { name: 'Message' }), 'Stalled');
    await user.click(screen.getByRole('button', { name: 'Send message' }));
    await waitFor(() => expect(submitInput).toHaveBeenCalledTimes(1));

    const recent = within(screen.getByLabelText('Recent sessions'));
    await user.click(recent.getByRole('button', { name: /^Planning/ }));
    await waitFor(() => expect(events.connections[1]?.key).toBe('lobby/planning'));
    act(() => events.handlers[1].onSnapshot(planning));

    const input = await screen.findByRole('textbox', { name: 'Message' });
    await waitFor(() => expect(input).toBeEnabled());
    await user.type(input, 'Fresh conversation');
    await user.click(screen.getByRole('button', { name: 'Send message' }));

    await waitFor(() => expect(submitInput).toHaveBeenLastCalledWith(
      'lobby', 'planning', { text: 'Fresh conversation' },
    ));
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });

  // The workspace decides how a character is set; the browser only maps the
  // words it is given onto classes, and says nothing for a character that asked
  // for nothing.
  it('sets each character in its configured voice and leaves the reader alone', async () => {
    const events = drivableEvents();
    render(
      <App client={fixtureClient()} connectSessionEvents={events.connect} />,
    );
    await attachInitial(events, {
      ...snapshotFixture,
      characters: [
        {
          id: 'seneca',
          display_name: 'Seneca',
          appearance: {
            font: 'serif', style: 'italic', weight: 'semibold', size: 'large', text_color: 'accent',
          },
        },
        { id: 'assistant', display_name: 'Assistant', appearance: plainVoice },
      ],
      transcript: [
        {
          id: 1, kind: 'human', participant_id: 'guest', display_name: 'Guest',
          addressed_to: 'seneca', addressed_to_name: 'Seneca',
          text: 'A question', status: 'complete', created_at: null,
        },
        {
          id: 2, kind: 'character', participant_id: 'seneca', display_name: 'Seneca',
          addressed_to: 'guest', addressed_to_name: 'Guest',
          text: 'A considered answer', status: 'complete', created_at: null,
        },
        {
          id: 3, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
          addressed_to: 'guest', addressed_to_name: 'Guest',
          text: 'A plain answer', status: 'complete', created_at: null,
        },
      ],
    });

    expect(screen.getByText('A considered answer')).toHaveClass(
      'cha-message-text', 'cha-font-serif', 'cha-slant-italic', 'cha-weight-semibold',
      'cha-scale-large', 'cha-color-accent',
    );
    expect(screen.getByText('A considered answer')).not.toHaveClass('cha-weight-bold');
    expect(screen.getByText('A plain answer').className).toBe('cha-message-text');
    // The reader's own words are never in costume.
    expect(screen.getByText('A question').className).toBe('cha-message-text');
  });

  it('changes the target only after authoritative state confirms it', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    const setDefaultCharacter = vi.fn(async () => ({ clear_input: false }));
    const snapshot: SessionSnapshot = {
      ...snapshotFixture,
      forum: {
        ...snapshotFixture.forum,
        members: bootstrapFixture.characters,
      },
      characters: bootstrapFixture.characters,
    };
    render(
      <App
        client={fixtureClient({ setDefaultCharacter })}
        connectSessionEvents={events.connect}
      />,
    );
    await attachInitial(events, snapshot);

    await user.selectOptions(
      screen.getByRole('combobox', { name: 'Choose message target' }),
      'guide',
    );
    await waitFor(() => expect(setDefaultCharacter).toHaveBeenCalledWith(
      'entrance', 'welcome', 'guide',
    ));
    expect(screen.getByLabelText('Current chat context')).toHaveTextContent('To: Assistant');

    act(() => events.handlers[0].onSnapshot({ ...snapshot, default_character_id: 'guide' }));
    expect(screen.getByLabelText('Current chat context')).toHaveTextContent('To: Guide');
  });

  it('offers recording as a target and follows authoritative target changes', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    const setDefaultCharacter = vi.fn(async () => ({ clear_input: false }));
    const snapshot: SessionSnapshot = {
      ...snapshotFixture,
      characters: bootstrapFixture.characters,
    };
    render(
      <App
        client={fixtureClient({ setDefaultCharacter })}
        connectSessionEvents={events.connect}
      />,
    );
    await attachInitial(events, snapshot);

    const chooser = screen.getByRole('combobox', { name: 'Choose message target' });
    expect(within(chooser).getByRole('option', { name: 'Self-notes' })).toBeInTheDocument();
    await user.selectOptions(chooser, '-');
    await waitFor(() => expect(setDefaultCharacter).toHaveBeenCalledWith(
      'entrance', 'welcome', '-',
    ));
    expect(screen.getByLabelText('Current chat context')).toHaveTextContent('To: Assistant');

    act(() => events.handlers[0].onSnapshot({ ...snapshot, default_character_id: '-' }));
    expect(chooser).toHaveValue('-');
    expect(screen.getByRole('textbox', { name: 'Message' }))
      .toHaveAttribute('placeholder', 'Self-notes — saved, not sent');
    expect(screen.getByLabelText('Current chat context')).toHaveTextContent('To: Self-notes');

    // Choosing a real character leaves recording mode through the typed setter.
    await user.selectOptions(chooser, 'guide');
    await waitFor(() => expect(setDefaultCharacter).toHaveBeenCalledWith(
      'entrance', 'welcome', 'guide',
    ));

    act(() => events.handlers[0].onSnapshot({ ...snapshot, default_character_id: 'guide' }));
    await waitFor(() => expect(chooser).toHaveValue('guide'));
    expect(within(chooser).getByRole('option', { name: 'Self-notes' })).toBeInTheDocument();
    expect(screen.getByRole('textbox', { name: 'Message' }))
      .toHaveAttribute('placeholder', 'Message Guide');
    expect(screen.getByLabelText('Current chat context')).toHaveTextContent('To: Guide');
  });

  it('follows new text only while the reader is at the end of the transcript', async () => {
    const scrollIntoView = vi.fn();
    Object.defineProperty(HTMLElement.prototype, 'scrollIntoView', {
      configurable: true,
      value: scrollIntoView,
    });
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, transcriptSnapshot());

    const transcript = screen.getByLabelText('Conversation transcript');
    scrollIntoView.mockClear();
    act(() => events.handlers[0].onAppend({
      target: { kind: 'entry', entry_id: 4 }, text: ' one', seq: 0,
    }));
    expect(scrollIntoView).toHaveBeenCalled();

    placeReadingPosition(transcript, { scrollTop: 0, scrollHeight: 900, clientHeight: 300 });
    scrollIntoView.mockClear();
    act(() => events.handlers[0].onAppend({
      target: { kind: 'entry', entry_id: 4 }, text: ' two', seq: 1,
    }));
    expect(screen.getByText('Still here one two')).toBeInTheDocument();
    expect(scrollIntoView).not.toHaveBeenCalled();

    placeReadingPosition(transcript, { scrollTop: 600, scrollHeight: 900, clientHeight: 300 });
    act(() => events.handlers[0].onAppend({
      target: { kind: 'entry', entry_id: 4 }, text: ' three', seq: 2,
    }));
    expect(scrollIntoView).toHaveBeenCalled();
  });

  it('explains a session whose end arrives over a healthy stream', async () => {
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, transcriptSnapshot());

    act(() => events.handlers[0].onSnapshot({
      ...transcriptSnapshot(),
      generation: snapshotFixture.generation,
      lifecycle: 'stopping',
      shutdown_reason: 'server_stopping',
    }));

    expect(screen.getByRole('alert')).toHaveTextContent('CHA is shutting down');
    expect(screen.getByRole('textbox', { name: 'Message' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Return to Welcome' })).toBeInTheDocument();
    // The conversation it already has stays readable rather than going blank.
    expect(screen.getByText('Still here')).toBeInTheDocument();
  });

  it('explains a settings reload without offering recovery actions', async () => {
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, transcriptSnapshot());

    act(() => events.handlers[0].onSnapshot({
      ...transcriptSnapshot(),
      generation: snapshotFixture.generation,
      lifecycle: 'stopping',
      shutdown_reason: 'reloading',
    }));

    expect(screen.getByRole('status')).toHaveTextContent('Applying settings');
    expect(screen.queryByRole('button', { name: 'Retry' })).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Browse sessions' })).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Return to Welcome' })).not.toBeInTheDocument();
  });

  it('offers recovery actions when a settings reload never reopens', async () => {
    const events = drivableEvents();
    let snapshots = 0;
    const getSessionSnapshot = vi.fn(async () => {
      snapshots += 1;
      if (snapshots === 1) return transcriptSnapshot();
      throw new ChaError(500, 'internal_error', 'The request could not be completed.');
    });
    render(<App
      client={fixtureClient({ getSessionSnapshot })}
      connectSessionEvents={events.connect}
      retryDelays={[0]}
    />);
    await attachInitial(events, transcriptSnapshot());

    act(() => events.handlers[0].onSnapshot({
      ...transcriptSnapshot(),
      lifecycle: 'stopping',
      shutdown_reason: 'reloading',
    }));
    act(() => events.handlers[0].onError({ kind: 'stream_failure' }));

    // The ladder gives up while the last snapshot still says `reloading`, and
    // that stale reason must not withhold the only way back.
    expect(await screen.findByRole('alert')).toHaveTextContent(
      'Live updates could not be restored.',
    );
    expect(screen.getByRole('button', { name: 'Retry' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Return to Welcome' })).toBeInTheDocument();
  });

  it('keeps Stop visible until authoritative generation state becomes inactive', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    const stopGeneration = vi.fn(async () => ({ clear_input: false }));
    const active = transcriptSnapshot();
    render(
      <App
        client={fixtureClient({ stopGeneration })}
        connectSessionEvents={events.connect}
      />,
    );
    await attachInitial(events, active);

    await user.click(screen.getByRole('button', { name: 'Stop generation' }));
    expect(stopGeneration).toHaveBeenCalledWith('entrance', 'welcome');
    expect(screen.getByRole('button', { name: 'Stop generation' })).toBeInTheDocument();

    act(() => events.handlers[0].onSnapshot({
      ...active,
      generation: snapshotFixture.generation,
      transcript: [{ ...active.transcript[0], status: 'cancelled' }],
    }));
    expect(screen.getByRole('button', { name: 'Send message' })).toBeInTheDocument();
    expect(screen.getByText('Stopped')).toBeInTheDocument();
  });

  it('keeps draft editing and Stop available while live updates reconnect', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    const active = transcriptSnapshot();
    const stopGeneration = vi.fn(async () => ({ clear_input: false }));
    const getSessionSnapshot = vi.fn()
      .mockResolvedValueOnce(active)
      .mockImplementation(() => new Promise<SessionSnapshot>(() => undefined));
    render(
      <App
        client={fixtureClient({ getSessionSnapshot, stopGeneration })}
        connectSessionEvents={events.connect}
      />,
    );
    await attachInitial(events, active);

    act(() => events.handlers[0].onError({ kind: 'stream_failure' }));
    expect(await screen.findByRole('status')).toHaveTextContent('Reconnecting live updates');

    const input = screen.getByRole('textbox', { name: 'Message' });
    expect(input).toBeEnabled();
    await user.type(input, 'Draft while offline');
    expect(input).toHaveValue('Draft while offline');
    await user.click(screen.getByRole('button', { name: 'Stop generation' }));
    expect(stopGeneration).toHaveBeenCalledWith('entrance', 'welcome');
  });
});

describe('live stream recovery', () => {
  it('probes a live session, reconnects, and accepts a fresh stream snapshot', async () => {
    const events = drivableEvents();
    const getSessionSnapshot = vi.fn(async () => snapshotFixture);
    render(
      <App
        client={fixtureClient({ getSessionSnapshot })}
        connectSessionEvents={events.connect}
        retryDelays={[0]}
      />,
    );
    await attachInitial(events);
    act(() => events.handlers[0].onError({ kind: 'stream_failure' }));

    await waitFor(() => expect(events.connections).toHaveLength(2));
    expect(getSessionSnapshot).toHaveBeenCalledTimes(2);
    act(() => events.handlers[1].onSnapshot(snapshotFixture));
    await waitFor(() => expect(screen.queryByText(/Reconnecting live updates/)).not.toBeInTheDocument());
  });

  it('re-opens a session the server has unloaded before reconnecting', async () => {
    const events = drivableEvents();
    const openSession = vi.fn(async (forumId: string, sessionId: string) => ({
      forum_id: forumId,
      session_id: sessionId,
    }));
    const getSessionSnapshot = vi.fn()
      .mockResolvedValueOnce(snapshotFixture)
      .mockRejectedValueOnce(new ChaError(409, 'session_not_live', 'Session is not live.'))
      .mockResolvedValueOnce(snapshotFixture);
    render(
      <App
        client={fixtureClient({ getSessionSnapshot, openSession })}
        connectSessionEvents={events.connect}
        retryDelays={[0]}
      />,
    );
    await attachInitial(events);
    act(() => events.handlers[0].onError({ kind: 'stream_failure' }));

    await waitFor(() => expect(events.connections).toHaveLength(2));
    expect(openSession.mock.calls.filter(([, id]) => id === 'welcome')).toHaveLength(2);
    expect(getSessionSnapshot).toHaveBeenCalledTimes(3);
  });

  it('keeps temporary server failures in the bounded retry path', async () => {
    const events = drivableEvents();
    const getSessionSnapshot = vi.fn()
      .mockResolvedValueOnce(snapshotFixture)
      .mockRejectedValueOnce(new Error('Server unavailable'))
      .mockResolvedValueOnce(snapshotFixture);
    render(
      <App
        client={fixtureClient({ getSessionSnapshot })}
        connectSessionEvents={events.connect}
        retryDelays={[0, 0]}
      />,
    );
    await attachInitial(events);
    act(() => events.handlers[0].onError({ kind: 'stream_failure' }));

    await waitFor(() => expect(events.connections).toHaveLength(2));
    expect(getSessionSnapshot).toHaveBeenCalledTimes(3);
  });

  // The reader opened this conversation on another device. Reconnecting would
  // take it straight back, so this page parks with the transcript it has and
  // waits to be asked.
  it('parks the page when another device takes the session over', async () => {
    const events = drivableEvents();
    const snapshot = transcriptSnapshot();
    const getSessionSnapshot = vi.fn(async () => snapshot);
    render(
      <App
        client={fixtureClient({ getSessionSnapshot })}
        connectSessionEvents={events.connect}
        retryDelays={[0, 0]}
      />,
    );
    await attachInitial(events, snapshot);
    const probesBefore = getSessionSnapshot.mock.calls.length;
    act(() => events.handlers[0].onError({ kind: 'superseded' }));

    expect(await screen.findByRole('alert')).toHaveTextContent(
      'This conversation moved to another device',
    );
    expect(screen.getByText('Still here')).toBeInTheDocument();
    expect(events.connections[0].close).toHaveBeenCalled();
    // No probe and no new stream: the ladder never ran.
    expect(getSessionSnapshot.mock.calls).toHaveLength(probesBefore);
    expect(events.connections).toHaveLength(1);
  });

  // A stream that has already reconnected once carries a late-failure callback
  // from its ladder rung. A takeover must park that page too, not hand it back
  // to recovery.
  it('parks a reconnected page when another device takes the session over', async () => {
    const events = drivableEvents();
    const snapshot = transcriptSnapshot();
    render(
      <App
        client={fixtureClient({ getSessionSnapshot: async () => snapshot })}
        connectSessionEvents={events.connect}
        retryDelays={[0, 0]}
      />,
    );
    await attachInitial(events, snapshot);
    act(() => events.handlers[0].onError({ kind: 'stream_failure' }));
    await waitFor(() => expect(events.connections).toHaveLength(2));
    act(() => events.handlers[1].onSnapshot(snapshot));

    act(() => events.handlers[1].onError({ kind: 'superseded' }));

    expect(await screen.findByRole('alert')).toHaveTextContent(
      'This conversation moved to another device',
    );
    await new Promise((resolve) => setTimeout(resolve, 20));
    expect(events.connections).toHaveLength(2);
  });

  it('takes a moved session back when the reader continues here', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    const snapshot = transcriptSnapshot();
    render(
      <App
        client={fixtureClient({ getSessionSnapshot: async () => snapshot })}
        connectSessionEvents={events.connect}
        retryDelays={[0]}
      />,
    );
    await attachInitial(events, snapshot);
    act(() => events.handlers[0].onError({ kind: 'superseded' }));
    await screen.findByRole('button', { name: 'Continue here' });

    await user.click(screen.getByRole('button', { name: 'Continue here' }));

    await waitFor(() => expect(events.connections).toHaveLength(2));
  });

  it('stops after bounded probe failures with an explicit Retry action', async () => {
    const events = drivableEvents();
    let snapshots = 0;
    const client = fixtureClient({
      getSessionSnapshot: async () => {
        snapshots += 1;
        if (snapshots === 1) return snapshotFixture;
        throw new Error('Server unavailable');
      },
    });
    render(
      <App
        client={client}
        connectSessionEvents={events.connect}
        retryDelays={[0, 0, 0, 0, 0]}
      />,
    );
    await attachInitial(events);
    act(() => events.handlers[0].onError({ kind: 'stream_failure' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('Live updates could not be restored');
    expect(screen.getByRole('button', { name: 'Retry' })).toBeInTheDocument();
    expect(snapshots).toBe(6);
  });
});

describe('live session capacity', () => {
  function capacityError() {
    return new ChaError(503, 'session_limit_reached', 'Session limit reached.');
  }

  it('retries a transient session limit and eventually attaches', async () => {
    const events = drivableEvents();
    const openSession = vi.fn()
      .mockRejectedValueOnce(capacityError())
      .mockRejectedValueOnce(capacityError())
      .mockResolvedValueOnce({ forum_id: 'entrance', session_id: 'welcome' });
    render(
      <App
        client={fixtureClient({ openSession })}
        connectSessionEvents={events.connect}
        retryDelays={[0, 0]}
      />,
    );

    await waitFor(() => expect(events.connections).toHaveLength(1));
    expect(openSession).toHaveBeenCalledTimes(3);
  });

  it('offers Retry and Return to Welcome after the session-limit bound is exhausted', async () => {
    window.history.replaceState(null, '', '/s/lobby/planning/');
    const openSession = vi.fn(async () => { throw capacityError(); });
    const client: ChaClient = fixtureClient({ openSession });
    render(<App client={client} retryDelays={[0, 0]} />);

    expect(await screen.findByRole('heading', { name: 'Session unavailable' })).toBeInTheDocument();
    expect(screen.getByRole('alert')).toHaveTextContent('Another session has not closed yet');
    expect(screen.getByRole('button', { name: 'Retry' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Return to Welcome' })).toBeInTheDocument();
    expect(openSession).toHaveBeenCalledTimes(3);
  });

  it('retries opening a newly created session without creating it twice', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    const createSession = vi.fn(async () => ({ id: 'created', label: 'Created once' }));
    let createdOpens = 0;
    const openSession = vi.fn(async (forumId: string, sessionId: string) => {
      if (sessionId === 'created' && (createdOpens += 1) <= 2) throw capacityError();
      return { forum_id: forumId, session_id: sessionId };
    });
    const lobbySnapshot: SessionSnapshot = {
      ...snapshotFixture,
      forum: bootstrapFixture.forums[1],
      session_id: 'created',
      session_label: 'Created once',
      characters: [bootstrapFixture.characters[1]],
      default_character_id: 'guide',
    };
    render(
      <App
        client={fixtureClient({
          createSession,
          getSessionSnapshot: async (forumId) => forumId === 'lobby'
            ? lobbySnapshot
            : snapshotFixture,
          listSessions: async () => [],
          openSession,
        })}
        connectSessionEvents={events.connect}
        retryDelays={[0]}
      />,
    );

    await user.click(await screen.findByRole('button', { name: 'Forums' }));
    await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
    await user.click(await screen.findByRole('button', { name: /New session/ }));
    await user.type(screen.getByRole('textbox', { name: 'Session name' }), 'Created once');
    await user.click(screen.getByRole('button', { name: 'Start session' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('Another session has not closed yet');
    expect(screen.getByRole('button', { name: 'Start session' })).toBeDisabled();
    await user.click(screen.getByRole('button', { name: 'Retry' }));

    await waitFor(() => expect(events.connections.some(({ key }) => key === 'lobby/created')).toBe(true));
    expect(createSession).toHaveBeenCalledTimes(1);
    expect(openSession.mock.calls.filter(([, id]) => id === 'created')).toHaveLength(3);
  });
});
