import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { ChaError, type AudioDownloadAcceptance, type AudioDownloadBatchAcceptance, type AudioDownloadBatchRequest, type ChaClient, type MediaResource, type SessionSnapshot } from '../api/client';
import type { SessionEventHandlers } from '../api/events';
import {
  bootstrapFixture,
  fixtureClient,
  monoLargeVoice,
  plainVoice,
  serifItalicVoice,
  snapshotFixture,
  voiceOutputRuntimeFixture,
} from '../test/fixtures';
import {
  TextToSpeechError,
  TextToSpeechSession,
} from '../textToSpeech';
import { VoiceInputSession } from '../voiceInput';
import { App } from './App';
import { formatEntryTime } from './ChatScreen';

afterEach(() => {
  vi.unstubAllGlobals();
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
    expect(await screen.findByLabelText('Chat area')).toHaveTextContent('Welcome');
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

  it('offers text to speech for stored prompts and completed model responses', async () => {
    const play = vi.spyOn(TextToSpeechSession.prototype, 'play').mockResolvedValue();
    const stop = vi.spyOn(TextToSpeechSession.prototype, 'stop');
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
    })} connectSessionEvents={events.connect} />);
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

    fireEvent.click(screen.getByRole('button', { name: 'Generate audio for your prompt' }));
    await waitFor(() => expect(play).toHaveBeenCalledOnce());
    fireEvent.click(await screen.findByRole('button', { name: 'Stop reading your prompt' }));
    expect(stop).toHaveBeenCalledOnce();
    fireEvent.click(screen.getByRole('button', { name: "Generate audio for Assistant's response" }));
    await waitFor(() => expect(play).toHaveBeenCalledTimes(2));
    fireEvent.click(await screen.findByRole('button', {
      name: "Stop reading Assistant's response",
    }));
    expect(stop).toHaveBeenCalledTimes(2);
  });

  it('resumes stopped responses independently and starts over after playback ends', async () => {
    vi.spyOn(globalThis, 'fetch').mockImplementation(async () => new Response('audio', {
      headers: { 'X-CHA-Audio-Cached': 'true' },
    }));
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
    const audios: Array<EventTarget & { currentTime: number; duration: number; readyState: number }> = [];
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      const audio = Object.assign(new EventTarget(), {
        currentTime: 0, duration: 60, readyState: 1,
        pause: vi.fn(), play: vi.fn().mockResolvedValue(undefined),
      });
      audios.push(audio);
      return audio;
    }));
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: [
        { id: 1, kind: 'human', participant_id: 'guest', display_name: 'Guest',
          addressed_to: 'assistant', addressed_to_name: 'Assistant', text: 'Question',
          status: 'complete', created_at: 1_700_000_000 },
        { id: 2, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
          addressed_to: '', addressed_to_name: '', text: 'Answer',
          status: 'complete', created_at: 1_700_000_001 },
      ],
    });
    fireEvent.click(screen.getByRole('button', { name: "Generate audio for Assistant's response" }));
    await screen.findByRole('button', { name: "Stop reading Assistant's response" });
    audios[0].currentTime = 18.25;
    fireEvent.click(screen.getByRole('button', { name: 'Generate audio for your prompt' }));
    await screen.findByRole('button', { name: 'Stop reading your prompt' });
    expect(audios[1].currentTime).toBe(0);
    audios[1].currentTime = 5;

    fireEvent.click(screen.getByRole('button', { name: "Play cached audio for Assistant's response" }));
    fireEvent.click(await screen.findByRole('button', { name: "Stop reading Assistant's response" }));
    expect(audios[2].currentTime).toBe(18.25);
    fireEvent.click(screen.getByRole('button', { name: 'Play cached audio for your prompt' }));
    expect(await screen.findByRole('button', { name: 'Stop reading your prompt' })).toBeInTheDocument();
    expect(audios[3].currentTime).toBe(5);
    act(() => audios[3].dispatchEvent(new Event('ended')));

    fireEvent.click(screen.getByRole('button', { name: 'Play cached audio for your prompt' }));
    await screen.findByRole('button', { name: 'Stop reading your prompt' });
    expect(audios[4].currentTime).toBe(0);
  });

  it('shows cached audio after a committed download', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response('audio', {
      headers: {},
    }));
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return { addEventListener: vi.fn(), pause: vi.fn(), play: vi.fn().mockResolvedValue(undefined) };
    }));
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: [{
        id: 2, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
        addressed_to: '', addressed_to_name: '', text: 'Answer', status: 'complete',
        created_at: 1_700_000_001,
      }],
    });

    const generate = screen.getByRole('button', { name: "Generate audio for Assistant's response" });
    expect(generate).toHaveAttribute('title', 'Generate audio');
    expect(generate).not.toHaveClass('has-cached-audio');
    fireEvent.click(generate);
    fireEvent.click(await screen.findByRole('button', { name: "Stop reading Assistant's response" }));

    const idle = screen.getByRole('button', {
      name: "Play cached audio for Assistant's response",
    });
    expect(idle).toHaveAttribute('title', 'Play cached audio');
    expect(idle.classList.contains('has-cached-audio')).toBe(true);
  });

  it('keeps both selected downloads running and autoplays only the latest selection', async () => {
    const play = vi.spyOn(TextToSpeechSession.prototype, 'play').mockResolvedValue();
    const completions = new Map<number, (value: AudioDownloadAcceptance) => void>();
    const cached: number[] = [];
    const startAudioDownload = vi.fn((_forum: string, _session: string, entryId: number) =>
      new Promise<AudioDownloadAcceptance>((resolve) => { completions.set(entryId, resolve); }));
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
      getAudioDownloads: async () => ({ cached_entry_ids: [...cached], downloads: [] }),
      startAudioDownload,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events, { ...snapshotFixture, transcript: [
      { id: 1, kind: 'human', participant_id: 'guest', display_name: 'Guest',
        addressed_to: 'assistant', addressed_to_name: 'Assistant', text: 'Question', status: 'complete', created_at: 1 },
      { id: 2, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
        addressed_to: '', addressed_to_name: '', text: 'Answer', status: 'complete', created_at: 2 },
    ] });
    fireEvent.click(screen.getByRole('button', { name: 'Generate audio for your prompt' }));
    expect(screen.getByRole('button', { name: 'Queued audio for your prompt' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: "Generate audio for Assistant's response" }));
    expect(startAudioDownload).toHaveBeenCalledTimes(2);
    await act(async () => { cached.push(1); completions.get(1)!({ entry_id: 1, cached: true }); });
    expect(play).not.toHaveBeenCalled();
    await act(async () => { cached.push(2); completions.get(2)!({ entry_id: 2, cached: true }); });
    await screen.findByRole('button', { name: "Stop reading Assistant's response" });
    expect(play).toHaveBeenCalledOnce();
    expect(screen.getByRole('button', { name: 'Play cached audio for your prompt' })).toBeEnabled();
  });

  it('reports a rejected earlier request once without canceling the latest audio selection', async () => {
    const play = vi.spyOn(TextToSpeechSession.prototype, 'play').mockResolvedValue();
    const completions = new Map<number, { resolve(value: AudioDownloadAcceptance): void; reject(error: unknown): void }>();
    const cached: number[] = [];
    const startAudioDownload = vi.fn((_forum: string, _session: string, entryId: number) =>
      new Promise<AudioDownloadAcceptance>((resolve, reject) => { completions.set(entryId, { resolve, reject }); }));
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
      getAudioDownloads: async () => ({ cached_entry_ids: [...cached], downloads: [] }),
      startAudioDownload,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events, { ...snapshotFixture, transcript: [
      { id: 1, kind: 'human', participant_id: 'guest', display_name: 'Guest',
        addressed_to: 'assistant', addressed_to_name: 'Assistant', text: 'Question', status: 'complete', created_at: 1 },
      { id: 2, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
        addressed_to: '', addressed_to_name: '', text: 'Answer', status: 'complete', created_at: 2 },
    ] });
    await waitFor(() => expect(screen.getByRole('button', { name: 'Generate audio for your prompt' })).toBeEnabled());
    fireEvent.click(screen.getByRole('button', { name: 'Generate audio for your prompt' }));
    fireEvent.click(screen.getByRole('button', { name: "Generate audio for Assistant's response" }));
    expect(startAudioDownload).toHaveBeenCalledTimes(2);
    await act(async () => { completions.get(1)!.reject(new ChaError('speech_busy', 'Audio downloads are temporarily unavailable.')); });
    expect(screen.getAllByRole('alert')).toHaveLength(1);
    expect(screen.getByRole('alert')).toHaveTextContent('Audio downloads are temporarily unavailable.');
    expect(screen.getByRole('button', { name: 'Generate audio for your prompt' })).toBeEnabled();
    expect(screen.getByRole('button', { name: "Queued audio for Assistant's response" })).toBeDisabled();
    await act(async () => { cached.push(2); completions.get(2)!.resolve({ entry_id: 2, cached: true }); });
    await screen.findByRole('button', { name: "Stop reading Assistant's response" });
    expect(play).toHaveBeenCalledOnce();
    expect(screen.getByRole('button', { name: 'Generate audio for your prompt' })).toBeEnabled();
  });

  it('generates with one click after a job disappears without cached audio or failure', async () => {
    const play = vi.spyOn(TextToSpeechSession.prototype, 'play').mockResolvedValue();
    const startAudioDownload = vi.fn(async () => ({ entry_id: 1, cached: false, state: 'queued' as const }));
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
      getAudioDownloads: async () => ({ cached_entry_ids: [], downloads: [] }),
      startAudioDownload,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events, { ...snapshotFixture, transcript: [
      { id: 1, kind: 'human', participant_id: 'guest', display_name: 'Guest',
        addressed_to: 'assistant', addressed_to_name: 'Assistant', text: 'Question', status: 'complete', created_at: 1 },
    ] });
    fireEvent.click(await screen.findByRole('button', { name: 'Generate audio for your prompt' }));
    await waitFor(() => expect(startAudioDownload).toHaveBeenCalledOnce());
    // The follow-up status read reports that maintenance canceled the job.
    fireEvent.click(await screen.findByRole('button', { name: 'Generate audio for your prompt' }));
    await waitFor(() => expect(startAudioDownload).toHaveBeenCalledTimes(2));
    expect(play).not.toHaveBeenCalled();
  });

  it('refreshes a native cached audio miss and offers generation without automatically starting it', async () => {
    const startAudioDownload = vi.fn();
    const getAudioDownloads = vi.fn().mockResolvedValueOnce({ cached_entry_ids: [2], downloads: [] })
      .mockResolvedValue({ cached_entry_ids: [], downloads: [] });
    const resolveAudioSource = vi.fn(async () => {
      throw new ChaError('not_found', 'Cached audio not found.');
    });
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
      getAudioDownloads, startAudioDownload, resolveAudioSource,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events, { ...snapshotFixture, transcript: [
      { id: 2, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
        addressed_to: '', addressed_to_name: '', text: 'Answer', status: 'complete', created_at: 2, has_cached_audio: true },
    ] });
    fireEvent.click(await screen.findByRole('button', { name: "Play cached audio for Assistant's response" }));
    await screen.findByRole('button', { name: "Generate audio for Assistant's response" });
    expect(resolveAudioSource).toHaveBeenCalledWith('entrance', 'welcome', 2, 'Personal');
    expect(startAudioDownload).not.toHaveBeenCalled();
    expect(getAudioDownloads).toHaveBeenCalledTimes(2);
  });

  it('discards audio resolution completed for the previous conversation', async () => {
    const play = vi.spyOn(TextToSpeechSession.prototype, 'play').mockResolvedValue();
    let resolvePrevious!: (resource: MediaResource) => void;
    let resolveCurrent!: (resource: MediaResource) => void;
    const previous = new Promise<MediaResource>((resolve) => { resolvePrevious = resolve; });
    const current = new Promise<MediaResource>((resolve) => { resolveCurrent = resolve; });
    const resolveAudioSource = vi.fn()
      .mockReturnValueOnce(previous)
      .mockReturnValueOnce(current);
    const releaseResource = vi.fn(async () => undefined);
    const entry = {
      id: 2, kind: 'character' as const, participant_id: 'assistant', display_name: 'Assistant',
      addressed_to: '', addressed_to_name: '', text: 'Saved answer', status: 'complete' as const,
      created_at: 2, has_cached_audio: true,
    };
    const planning: SessionSnapshot = {
      ...snapshotFixture,
      forum: bootstrapFixture.forums[1],
      session_id: 'planning',
      session_label: 'Planning',
      transcript: [entry],
    };
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getSessionSnapshot: async (forumId) => forumId === 'lobby' ? planning : snapshotFixture,
      getAudioDownloads: async () => ({ cached_entry_ids: [2], downloads: [] }),
      resolveAudioSource,
      releaseResource,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events, { ...snapshotFixture, transcript: [entry] });

    fireEvent.click(await screen.findByRole('button', {
      name: "Play cached audio for Assistant's response",
    }));
    fireEvent.click(screen.getByRole('button', { name: /^Planning/ }));
    await waitFor(() => expect(events.connections[1]?.key).toBe('lobby/planning'));
    act(() => events.handlers[1].onSnapshot(planning));
    fireEvent.click(await screen.findByRole('button', {
      name: "Play cached audio for Assistant's response",
    }));

    await act(async () => resolvePrevious({
      resource_id: 'old', url: '/media/old', mime_type: 'audio/mpeg', byte_length: 3,
    }));
    await waitFor(() => expect(releaseResource).toHaveBeenCalledWith('old'));
    expect(play).not.toHaveBeenCalled();

    await act(async () => resolveCurrent({
      resource_id: 'current', url: '/media/current', mime_type: 'audio/mpeg', byte_length: 3,
    }));
    await waitFor(() => expect(play).toHaveBeenCalledOnce());
  });

  it('plays committed audio when no synthesis configuration or key is available', async () => {
    const fetcher = vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response('audio'));
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:cached');
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return { addEventListener: vi.fn(), pause: vi.fn(), play: vi.fn().mockResolvedValue(undefined) };
    }));
    const startAudioDownload = vi.fn();
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => null,
      getAudioDownloads: async () => ({ cached_entry_ids: [2], downloads: [] }),
      startAudioDownload,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events, { ...snapshotFixture, transcript: [
      { id: 2, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
        addressed_to: '', addressed_to_name: '', text: 'Saved answer', status: 'complete', created_at: 2 },
    ] });
    fireEvent.click(await screen.findByRole('button', { name: "Play cached audio for Assistant's response" }));
    await screen.findByRole('button', { name: "Stop reading Assistant's response" });
    expect(fetcher).toHaveBeenCalledWith(
      '/media/cached-2', expect.any(Object));
    expect(startAudioDownload).not.toHaveBeenCalled();
  });

  it('shows saved audio after opening a session and resets its speaker color when the cache is cleared', async () => {
    vi.spyOn(globalThis, 'fetch').mockImplementation(async () => new Response('audio', {
      headers: { 'X-CHA-Audio-Cached': 'true' },
    }));
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
    const audios: Array<EventTarget & { currentTime: number }> = [];
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      const audio = Object.assign(new EventTarget(), {
        currentTime: 0, duration: 60, readyState: 1,
        pause: vi.fn(), play: vi.fn().mockResolvedValue(undefined),
      });
      audios.push(audio);
      return audio;
    }));
    const events = drivableEvents();
    let cachedIds = [2];
    const clearSessionAudioCache = vi.fn(async () => { cachedIds = []; });
    const saved: SessionSnapshot = {
      ...snapshotFixture, forum: bootstrapFixture.forums[1],
      session_id: 'planning', session_label: 'Planning',
      transcript: [{
        id: 2, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
        addressed_to: '', addressed_to_name: '', text: 'Saved answer', status: 'complete',
        created_at: 1_700_000_001, has_cached_audio: true,
      }],
    };
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
      getSessionSnapshot: async () => saved,
      clearSessionAudioCache,
      getAudioDownloads: async (forum) => ({ cached_entry_ids: forum === 'lobby' ? cachedIds : [], downloads: [] }),
      startAudioDownload: async (_forum, _session, entry_id) => { cachedIds = [entry_id]; return { entry_id, cached: true }; },
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events);
    fireEvent.click(screen.getByRole('button', { name: /^Planning/ }));
    await waitFor(() => expect(events.connections[1]?.key).toBe('lobby/planning'));
    act(() => events.handlers[1].onSnapshot(saved));

    const cached = await screen.findByRole('button', { name: "Play cached audio for Assistant's response" });
    expect(cached).toHaveClass('has-cached-audio');
    fireEvent.click(cached);
    const stop = await screen.findByRole('button', { name: "Stop reading Assistant's response" });
    audios[0].currentTime = 25;
    fireEvent.click(stop);
    fireEvent.click(screen.getByLabelText('Actions for Planning'));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Clear audio cache' }));
    await waitFor(() => expect(clearSessionAudioCache).toHaveBeenCalledWith('lobby', 'planning'));
    const generate = await screen.findByRole('button', { name: "Generate audio for Assistant's response" });
    expect(generate).not.toHaveClass('has-cached-audio');
    expect(screen.getByText('Saved answer')).toBeInTheDocument();
    fireEvent.click(generate);
    await screen.findByRole('button', { name: "Stop reading Assistant's response" });
    expect(audios[1].currentTime).toBe(0);
  });

  it('uses each stored prompt author\'s style and voice after the forum persona changes', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(
      new Response(new TextEncoder().encode('audio')),
    );
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return {
        addEventListener: vi.fn(),
        pause: vi.fn(),
        play: vi.fn().mockResolvedValue(undefined),
      };
    }));
    const startAudioDownload = vi.fn(async (_forum: string, _session: string, entry_id: number) => ({ entry_id, cached: true }));
    const events = drivableEvents();
    const personaVoice = {
      id: 'reader-voice',
      display_name: 'Reader voice',
      elevenlabs_voice_id: 'reader-elevenlabs',
      settings: {},
    };
    const bootstrap = {
      ...bootstrapFixture,
      personas: bootstrapFixture.personas.map((persona) => persona.id === 'reader'
        ? { ...persona, appearance: serifItalicVoice, voice: personaVoice }
        : persona),
    };
    render(<App client={fixtureClient({
      getBootstrap: async () => bootstrap,
      startAudioDownload,
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: [{
        id: 1, kind: 'human', participant_id: 'reader', display_name: 'Reader',
        addressed_to: 'assistant', addressed_to_name: 'Assistant',
        text: 'Styled question', status: 'complete', created_at: 1_700_000_000,
      }],
    });

    expect(screen.getByText('Styled question')).toHaveClass(
      'cha-font-serif', 'cha-slant-italic',
    );
    fireEvent.click(screen.getByRole('button', { name: 'Generate audio for your prompt' }));
    await waitFor(() => expect(startAudioDownload).toHaveBeenCalledWith(
      'entrance', 'welcome', 1, {
        vault_name: bootstrap.vault_name, reference_id: 'reader-elevenlabs', settings: {},
      },
    ));
  });

  it('submits a long conversation in one batch while prompts remain available and serializes later batches', async () => {
    let complete!: (accepted: AudioDownloadBatchAcceptance) => void;
    const jobs: Array<{ entry_id: number; state: 'queued' }> = [];
    const startAudioDownload = vi.fn();
    const startAudioDownloadBatch = vi.fn((_forum: string, _session: string, request: AudioDownloadBatchRequest) => {
      if (request.entries.length === 300) return new Promise<AudioDownloadBatchAcceptance>((resolve) => { complete = resolve; });
      const entries = request.entries.map(({ entry_id }) => ({ entry_id, cached: false, state: 'queued' as const }));
      jobs.push(...entries);
      return Promise.resolve({ entries });
    });
    const submitInput = vi.fn(async () => ({ clear_input: true }));
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
      getAudioDownloads: async () => ({ cached_entry_ids: [], downloads: [...jobs] }),
      startAudioDownload, startAudioDownloadBatch, submitInput,
    })} connectSessionEvents={events.connect} />);
    const entry = { id: 1, kind: 'character' as const, participant_id: 'assistant', display_name: 'Assistant',
      addressed_to: '', addressed_to_name: '', text: 'Answer', status: 'complete' as const, created_at: 1 };
    const snapshot = { ...snapshotFixture, transcript: Array.from({ length: 300 }, (_, index) =>
      ({ ...entry, id: index + 1, text: `Answer ${index + 1}` })) };
    await attachInitial(events, snapshot);
    const toggle = await screen.findByRole('button', { name: 'Cache conversation audio automatically' });
    await waitFor(() => expect(toggle).toBeEnabled());
    fireEvent.click(toggle);
    await waitFor(() => expect(startAudioDownloadBatch).toHaveBeenCalledOnce());
    expect(startAudioDownloadBatch.mock.calls[0][2].entries).toHaveLength(300);
    expect(startAudioDownload).not.toHaveBeenCalled();
    fireEvent.change(screen.getByRole('textbox', { name: 'Message' }), { target: { value: 'Another prompt' } });
    fireEvent.click(screen.getByRole('button', { name: 'Send message' }));
    await waitFor(() => expect(submitInput).toHaveBeenCalledWith('entrance', 'welcome', { text: 'Another prompt' }));
    const updated = { ...snapshot, transcript: [...snapshot.transcript, { ...entry, id: 301, text: 'New answer' }] };
    act(() => events.handlers[0].onSnapshot(updated));
    expect(startAudioDownloadBatch).toHaveBeenCalledOnce();
    await act(async () => {
      jobs.push(...snapshot.transcript.map(({ id }) => ({ entry_id: id, state: 'queued' as const })));
      complete({ entries: jobs.map((job) => ({ ...job, cached: false })) });
    });
    await waitFor(() => expect(startAudioDownloadBatch).toHaveBeenCalledTimes(2));
    expect(startAudioDownloadBatch.mock.lastCall?.[2].entries.map((item) => item.entry_id)).toEqual([301]);
    act(() => events.handlers[0].onSnapshot({ ...updated, transcript: [...updated.transcript] }));
    expect(startAudioDownloadBatch).toHaveBeenCalledTimes(2);
  });

  it('disables automatic caching after a permanent initial status failure without submitting entries', async () => {
    const startAudioDownloadBatch = vi.fn();
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
      getAudioDownloads: async () => { throw new ChaError('vault_changed', 'The active vault changed.'); },
      startAudioDownloadBatch,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events, { ...snapshotFixture, transcript: [
      { id: 1, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
        addressed_to: '', addressed_to_name: '', text: 'Answer', status: 'complete', created_at: 1 },
    ] });
    const toggle = await screen.findByRole('button', { name: 'Cache conversation audio automatically' });
    await waitFor(() => expect(toggle).toHaveAttribute('title', 'The active vault changed.'));
    expect(toggle).toBeDisabled();
    fireEvent.click(toggle);
    expect(startAudioDownloadBatch).not.toHaveBeenCalled();
  });

  it('clearing the session audio cache turns automatic caching off until explicitly re-enabled', async () => {
    let cached = [1];
    const events = drivableEvents();
    const startAudioDownloadBatch = vi.fn(async (_forum: string, _session: string, request: AudioDownloadBatchRequest) => {
      cached = request.entries.map((entry) => entry.entry_id);
      return { entries: cached.map((entry_id) => ({ entry_id, cached: true })) };
    });
    const saved: SessionSnapshot = { ...snapshotFixture, forum: bootstrapFixture.forums[1],
      session_id: 'planning', session_label: 'Planning', transcript: [
        { id: 1, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
          addressed_to: '', addressed_to_name: '', text: 'Saved answer', status: 'complete', created_at: 1 },
      ] };
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
      getSessionSnapshot: async () => saved,
      getAudioDownloads: async (forum) => ({ cached_entry_ids: forum === 'lobby' ? cached : [], downloads: [] }),
      clearSessionAudioCache: async () => { cached = []; },
      startAudioDownloadBatch,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events);
    fireEvent.click(screen.getByRole('button', { name: /^Planning/ }));
    await waitFor(() => expect(events.connections[1]?.key).toBe('lobby/planning'));
    act(() => events.handlers[1].onSnapshot(saved));
    const toggle = await screen.findByRole('button', { name: 'Cache conversation audio automatically' });
    await waitFor(() => expect(toggle).toBeEnabled());
    fireEvent.click(toggle);
    expect(toggle).toHaveAttribute('aria-pressed', 'true');
    fireEvent.click(screen.getByLabelText('Actions for Planning'));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Clear audio cache' }));
    await waitFor(() => expect(toggle).toHaveAttribute('aria-pressed', 'false'));
    await waitFor(() => expect(toggle).toBeEnabled());
    expect(startAudioDownloadBatch).not.toHaveBeenCalled();
    expect(screen.getByRole('button', { name: "Generate audio for Assistant's response" })).toBeEnabled();
    fireEvent.click(toggle);
    await waitFor(() => expect(startAudioDownloadBatch).toHaveBeenCalledOnce());
    expect(startAudioDownloadBatch.mock.calls[0][2].entries.map((entry) => entry.entry_id)).toEqual([1]);
  });

  it('automatically queues completed conversation audio once, using the entry voices and core jobs', async () => {
    const play = vi.spyOn(TextToSpeechSession.prototype, 'play');
    const jobs: Array<{ entry_id: number; state: 'queued' | 'running' | 'failed'; error?: string }> = [
      { entry_id: 8, state: 'running' }, { entry_id: 9, state: 'failed', error: 'Audio download failed. Try again.' },
    ];
    const startAudioDownload = vi.fn(async (_forum: string, _session: string, entry_id: number) => {
      jobs.push({ entry_id, state: 'queued' });
      return { entry_id, cached: false, state: 'queued' as const };
    });
    const bootstrap = { ...bootstrapFixture, personas: bootstrapFixture.personas.map((persona) =>
      persona.id === 'guest' ? { ...persona, voice: { id: 'reader', display_name: 'Reader',
        elevenlabs_voice_id: 'persona-voice', settings: { speed: 1.2 } } } : persona) };
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getBootstrap: async () => bootstrap,
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
      getAudioDownloads: async () => ({ cached_entry_ids: [1], downloads: [...jobs] }),
      startAudioDownload,
    })} connectSessionEvents={events.connect} />);
    const entry = { id: 1, kind: 'character' as const, participant_id: 'assistant', display_name: 'Assistant',
      addressed_to: '', addressed_to_name: '', text: 'Saved answer', status: 'complete' as const, created_at: 1 };
    const snapshot: SessionSnapshot = { ...snapshotFixture,
      characters: [{ ...snapshotFixture.characters[0], voice: { id: 'speaker', display_name: 'Speaker',
        elevenlabs_voice_id: 'character-voice', settings: { speed: 0.8 } } }],
      transcript: [entry,
        { ...entry, id: 2, kind: 'human', participant_id: 'guest', display_name: 'Guest', text: 'Question' },
        { ...entry, id: 3, text: 'Existing answer' },
        { ...entry, id: 4, text: 'Streaming answer', status: 'streaming', created_at: null },
        { ...entry, id: 5, status: 'failed', text: 'Failed answer' },
        { ...entry, id: 6, kind: 'notice', text: 'Notice' },
        { ...entry, id: 7, text: '   ' },
        { ...entry, id: 8, text: 'Already downloading' },
        { ...entry, id: 9, text: 'Failed download' },
      ] };
    await attachInitial(events, snapshot);
    const toggle = await screen.findByRole('button', { name: 'Cache conversation audio automatically' });
    await waitFor(() => expect(toggle).toBeEnabled());
    expect(toggle).toHaveAttribute('aria-pressed', 'false');
    expect(startAudioDownload).not.toHaveBeenCalled();
    fireEvent.click(toggle);
    await waitFor(() => expect(startAudioDownload).toHaveBeenCalledTimes(2));
    expect(startAudioDownload).toHaveBeenCalledWith('entrance', 'welcome', 2,
      { vault_name: 'Personal', reference_id: 'persona-voice', settings: { speed: 1.2 } });
    expect(startAudioDownload).toHaveBeenCalledWith('entrance', 'welcome', 3,
      { vault_name: 'Personal', reference_id: 'character-voice', settings: { speed: 0.8 } });
    expect(toggle).toHaveAttribute('aria-pressed', 'true');
    const completed = { ...snapshot, transcript: snapshot.transcript.map((item) => item.id === 4
      ? { ...item, status: 'complete' as const, created_at: 2 } : item) };
    act(() => events.handlers[0].onSnapshot(completed));
    await waitFor(() => expect(startAudioDownload).toHaveBeenCalledTimes(3));
    act(() => events.handlers[0].onSnapshot({ ...completed, transcript: [...completed.transcript] }));
    expect(startAudioDownload).toHaveBeenCalledTimes(3);
    expect(play).not.toHaveBeenCalled();
  });

  it('turning automatic audio off preserves cached audio and allows accepted downloads to finish', async () => {
    const clearSessionAudioCache = vi.fn(async () => {});
    const play = vi.spyOn(TextToSpeechSession.prototype, 'play');
    const cached = [1];
    let finish!: (value: AudioDownloadAcceptance) => void;
    const startAudioDownload = vi.fn((_forum: string, _session: string, entry_id: number) =>
      new Promise<AudioDownloadAcceptance>((resolve) => { finish = resolve; }));
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
      getAudioDownloads: async () => ({ cached_entry_ids: [...cached], downloads: [] }),
      startAudioDownload, clearSessionAudioCache,
    })} connectSessionEvents={events.connect} />);
    const entry = { id: 1, kind: 'character' as const, participant_id: 'assistant', display_name: 'Assistant',
      addressed_to: '', addressed_to_name: '', text: 'Saved answer', status: 'complete' as const, created_at: 1 };
    const snapshot = { ...snapshotFixture, transcript: [entry, { ...entry, id: 2, text: 'Existing answer' }] };
    await attachInitial(events, snapshot);
    const toggle = await screen.findByRole('button', { name: 'Cache conversation audio automatically' });
    await waitFor(() => expect(toggle).toBeEnabled());
    fireEvent.click(toggle);
    await waitFor(() => expect(startAudioDownload).toHaveBeenCalledOnce());
    expect(screen.getByRole('button', { name: "Queued audio for Assistant's response" })).toBeDisabled();
    fireEvent.click(toggle);
    expect(toggle).toHaveAttribute('aria-pressed', 'false');
    act(() => events.handlers[0].onSnapshot({ ...snapshot,
      transcript: [...snapshot.transcript, { ...entry, id: 3, text: 'New answer' }] }));
    await act(async () => { cached.push(2); finish({ entry_id: 2, cached: true }); });
    expect(screen.getAllByRole('button', { name: "Play cached audio for Assistant's response" })).toHaveLength(2);
    expect(screen.getByRole('button', { name: "Generate audio for Assistant's response" })).toBeEnabled();
    expect(startAudioDownload).toHaveBeenCalledOnce();
    expect(clearSessionAudioCache).not.toHaveBeenCalled();
    expect(play).not.toHaveBeenCalled();
    // Re-enabling queues only the answer that appeared while disabled.
    fireEvent.click(toggle);
    await waitFor(() => expect(startAudioDownload).toHaveBeenCalledTimes(2));
    expect(startAudioDownload.mock.lastCall?.[2]).toBe(3);
    await act(async () => { cached.push(3); finish({ entry_id: 3, cached: true }); });
  });

  it('turns automatic caching off after a rejected batch and retries eligible entries only when re-enabled', async () => {
    const jobs: Array<{ entry_id: number; state: 'queued' }> = [];
    const startAudioDownloadBatch = vi.fn(async (_forum: string, _session: string, request: AudioDownloadBatchRequest) => {
      const entries = request.entries.map(({ entry_id }) => ({ entry_id, cached: false, state: 'queued' as const }));
      jobs.push(...entries);
      return { entries };
    }).mockRejectedValueOnce(new ChaError('speech_busy', 'Audio downloads are temporarily unavailable.'));
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
      getAudioDownloads: async () => ({ cached_entry_ids: [], downloads: [...jobs] }),
      startAudioDownloadBatch,
    })} connectSessionEvents={events.connect} />);
    const entry = { id: 1, kind: 'character' as const, participant_id: 'assistant', display_name: 'Assistant',
      addressed_to: '', addressed_to_name: '', text: 'Answer', status: 'complete' as const, created_at: 1 };
    const snapshot = { ...snapshotFixture, transcript: [entry, { ...entry, id: 2, text: 'Another answer' }] };
    await attachInitial(events, snapshot);
    const toggle = await screen.findByRole('button', { name: 'Cache conversation audio automatically' });
    await waitFor(() => expect(toggle).toBeEnabled());
    fireEvent.click(toggle);
    expect(await screen.findByRole('alert')).toHaveTextContent('Audio downloads are temporarily unavailable.');
    expect(toggle).toHaveAttribute('aria-pressed', 'false');
    expect(startAudioDownloadBatch.mock.calls[0][2].entries.map((item) => item.entry_id)).toEqual([1, 2]);
    const updated = { ...snapshot, transcript: [...snapshot.transcript, { ...entry, id: 3, text: 'New answer' }] };
    act(() => events.handlers[0].onSnapshot(updated));
    expect(startAudioDownloadBatch).toHaveBeenCalledOnce();
    expect(screen.getAllByRole('alert')).toHaveLength(1);
    expect(screen.getAllByRole('button', { name: "Generate audio for Assistant's response" })).toHaveLength(3);
    fireEvent.click(toggle);
    await waitFor(() => expect(startAudioDownloadBatch).toHaveBeenCalledTimes(2));
    expect(startAudioDownloadBatch.mock.lastCall?.[2].entries.map((item) => item.entry_id)).toEqual([1, 2, 3]);
    expect(toggle).toHaveAttribute('aria-pressed', 'true');
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });

  it('resets automatic audio on session navigation and ignores late admission errors', async () => {
    let reject!: (failure: unknown) => void;
    const startAudioDownload = vi.fn(() => new Promise<AudioDownloadAcceptance>((_resolve, failed) => { reject = failed; }));
    const entry = { id: 1, kind: 'character' as const, participant_id: 'assistant', display_name: 'Assistant',
      addressed_to: '', addressed_to_name: '', text: 'Answer', status: 'complete' as const, created_at: 1 };
    const planning: SessionSnapshot = { ...snapshotFixture, forum: bootstrapFixture.forums[1],
      session_id: 'planning', session_label: 'Planning', transcript: [{ ...entry, text: 'Planning answer' }] };
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
      getSessionSnapshot: async (forum) => forum === 'lobby' ? planning : snapshotFixture,
      startAudioDownload,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events, { ...snapshotFixture, transcript: [entry] });
    const toggle = await screen.findByRole('button', { name: 'Cache conversation audio automatically' });
    await waitFor(() => expect(toggle).toBeEnabled());
    fireEvent.click(toggle);
    await waitFor(() => expect(startAudioDownload).toHaveBeenCalledOnce());
    fireEvent.click(screen.getByRole('button', { name: /^Planning/ }));
    await waitFor(() => expect(events.connections[1]?.key).toBe('lobby/planning'));
    act(() => events.handlers[1].onSnapshot(planning));
    await waitFor(() => expect(screen.getByRole('button', { name: 'Cache conversation audio automatically' })).toBeEnabled());
    expect(screen.getByRole('button', { name: 'Cache conversation audio automatically' })).toHaveAttribute('aria-pressed', 'false');
    await act(async () => { reject(new ChaError('speech_busy', 'Old audio request rejected.')); });
    expect(startAudioDownload).toHaveBeenCalledOnce();
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });

  it('shows an error message returned by FishAudio', async () => {
    vi.spyOn(TextToSpeechSession.prototype, 'play').mockRejectedValue(
      new TextToSpeechError('FishAudio: This voice is unavailable. (HTTP 422)'),
    );
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: [{
        id: 2,
        kind: 'character',
        participant_id: 'assistant',
        display_name: 'Assistant',
        addressed_to: '',
        addressed_to_name: '',
        text: 'Answer',
        status: 'complete',
        created_at: 1_700_000_001,
      }],
    });

    fireEvent.click(screen.getByRole('button', { name: "Generate audio for Assistant's response" }));

    expect(await screen.findByRole('alert')).toHaveTextContent(
      'FishAudio: This voice is unavailable. (HTTP 422)',
    );
  });

  it('shows a rejected audio request once and keeps generation available after status refresh', async () => {
    const getAudioDownloads = vi.fn(async () => ({ cached_entry_ids: [], downloads: [] }));
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture,
      getAudioDownloads,
      startAudioDownload: async () => { throw new ChaError('not_found', 'Audio admission rejected.'); },
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: [{ id: 2, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
        addressed_to: '', addressed_to_name: '', text: 'Answer', status: 'complete', created_at: 1_700_000_001 }],
    });
    fireEvent.click(screen.getByRole('button', { name: "Generate audio for Assistant's response" }));
    expect(await screen.findByRole('alert')).toHaveTextContent('Audio admission rejected.');
    await waitFor(() => expect(getAudioDownloads).toHaveBeenCalledTimes(2));
    expect(screen.getAllByText('Audio admission rejected.')).toHaveLength(1);
    expect(screen.getByRole('button', { name: "Generate audio for Assistant's response" })).toBeEnabled();
  });

  it('deletes a response with the prompt that generated it', async () => {
    const deleteTurn = vi.fn(async () => ({ clear_input: false }));
    const events = drivableEvents();
    const transcript: SessionSnapshot['transcript'] = [
      {
        id: 1, kind: 'human', participant_id: 'guest', display_name: 'Guest',
        addressed_to: 'assistant', addressed_to_name: 'Assistant',
        text: 'Question', status: 'complete', request_id: 5, created_at: 1_700_000_000,
      },
      {
        id: 2, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
        addressed_to: '', addressed_to_name: '',
        text: 'Answer', status: 'complete', request_id: 5, created_at: 1_700_000_001,
      },
    ];
    render(<App
      client={fixtureClient({ deleteTurn })}
      connectSessionEvents={events.connect}
    />);
    await attachInitial(events, { ...snapshotFixture, transcript });

    fireEvent.click(screen.getByRole('button', {
      name: "Delete Assistant's response and its prompt",
    }));
    const dialog = within(await screen.findByRole('dialog'));
    expect(dialog.getByText(/This cannot be undone/)).toBeInTheDocument();
    expect(deleteTurn).not.toHaveBeenCalled();
    fireEvent.click(dialog.getByRole('button', { name: 'Delete response' }));
    await waitFor(() => expect(deleteTurn).toHaveBeenCalledWith(
      'entrance', 'welcome', { response_entry_id: 2 },
    ));

    act(() => events.handlers[0].onSnapshot({
      ...snapshotFixture,
      transcript: [],
    }));
    expect(screen.queryByText('Question')).not.toBeInTheDocument();
    expect(screen.queryByText('Answer')).not.toBeInTheDocument();
  });

  it('keeps a response when deletion is cancelled', async () => {
    const deleteTurn = vi.fn(async () => ({ clear_input: false }));
    const events = drivableEvents();
    render(<App
      client={fixtureClient({ deleteTurn })}
      connectSessionEvents={events.connect}
    />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: [{
        id: 2, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
        addressed_to: '', addressed_to_name: '', text: 'Keep me', status: 'complete',
        request_id: 5, created_at: 1_700_000_001,
      }],
    });

    fireEvent.click(screen.getByRole('button', {
      name: "Delete Assistant's response and its prompt",
    }));
    fireEvent.click(within(await screen.findByRole('dialog')).getByRole('button', {
      name: 'Cancel',
    }));

    expect(deleteTurn).not.toHaveBeenCalled();
    expect(screen.getByText('Keep me')).toBeInTheDocument();
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
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
    const boundaryResponse = screen.getByText('Partial answer').closest('article');
    if (!boundaryResponse) throw new Error('Expected the boundary response article');
    expect(within(boundaryResponse).getByRole('button', { name: 'Uncover transcript' }))
      .toBeInTheDocument();
    expect(screen.getByRole('button', {
      name: "Cover transcript through Assistant's response",
    })).toBeInTheDocument();
    fireEvent.click(within(boundaryResponse).getByRole('button', { name: 'Uncover transcript' }));
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
    const fetchMock = vi.spyOn(globalThis, 'fetch');
    const events = drivableEvents();
    const client = fixtureClient({ getVoiceOutputRuntime: async () => voiceOutputRuntimeFixture });
    const startAudioDownload = vi.spyOn(client, 'startAudioDownload');
    render(<App client={client} connectSessionEvents={events.connect} />);
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

    const toggle = screen.getByRole('button', { name: 'Cache conversation audio automatically' });
    await waitFor(() => expect(toggle).toBeEnabled());
    fireEvent.click(toggle);
    await waitFor(() => expect(startAudioDownload).toHaveBeenCalledTimes(3));
    expect(startAudioDownload.mock.calls.map((call) => call[2])).toEqual([1, 2, 4]);
    expect(fetchMock).not.toHaveBeenCalled();
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
      .mockRejectedValueOnce(new ChaError('invalid_argument', 'The prompt was not accepted.'))
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
      .mockRejectedValueOnce(new ChaError('invalid_argument', 'The prompt was not accepted.'))
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
        client={fixtureClient({
          submitInput,
          getVoiceInputRuntime: async () => ({
            url: 'https://api.openai.com/v1/realtime/calls',
            model: 'gpt-live-transcribe',
            delay: 'high',
            prompt: 'Software design discussion.',
          }),
        })}
        connectSessionEvents={events.connect}
      />,
    );
    await attachInitial(events);

    const input = screen.getByRole('textbox', { name: 'Message' });
    fireEvent.click(screen.getByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    expect(startVoiceInput.mock.calls[0]?.[0].languages).toEqual(['en']);
    expect(startVoiceInput.mock.calls[0]?.[0]).toMatchObject({
      delay: 'high',
      prompt: 'Software design discussion.',
    });
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
    vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
    const voiceSession = {
      stop: vi.fn(async () => {}),
      cancel: vi.fn(),
    } as unknown as VoiceInputSession;
    const startVoiceInput = vi.spyOn(VoiceInputSession, 'start')
      .mockResolvedValue(voiceSession);
    const events = drivableEvents();
    render(<App client={fixtureClient({
      getVoiceInputRuntime: async () => ({
        url: 'https://api.openai.com/v1/realtime/calls',
        model: 'gpt-live-transcribe',
        delay: 'xhigh',
        prompt: 'Russian technical discussion.',
      }),
    })} connectSessionEvents={events.connect} />);
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
        {
          id: 4, kind: 'error', participant_id: 'seneca', display_name: 'Seneca',
          addressed_to: '', addressed_to_name: '', text: 'The response failed.',
          status: 'failed', created_at: 1_700_000_000,
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
    expect(screen.getByText('The response failed.').className).toBe('cha-message-text');
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

  it.each([
    { reason: 'server_stopping', message: 'CHA is shutting down' },
    { reason: 'session_closed', message: 'This session has closed. Its conversation is saved.' },
  ] as const)('explains $reason over a healthy stream', async ({ reason, message }) => {
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, transcriptSnapshot());

    act(() => events.handlers[0].onSnapshot({
      ...transcriptSnapshot(),
      generation: snapshotFixture.generation,
      lifecycle: 'stopping',
      shutdown_reason: reason,
    }));

    expect(screen.getByRole('alert')).toHaveTextContent(message);
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
    render(<App
      client={fixtureClient()}
      connectSessionEvents={events.connect}
    />);
    await attachInitial(events, transcriptSnapshot());

    act(() => events.handlers[0].onSnapshot({
      ...transcriptSnapshot(),
      lifecycle: 'stopping',
      shutdown_reason: 'reloading',
    }));
    act(() => events.handlers[0].onError({ kind: 'stream_failure' }));
    await waitFor(() => expect(events.connections).toHaveLength(2));
    act(() => events.handlers[1].onError({ kind: 'stream_failure' }));

    // A failed replacement while the last snapshot still says `reloading`
    // must not withhold the only way back.
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

  it('stops after one failed replacement with an explicit Retry action', async () => {
    const events = drivableEvents();
    render(
      <App
        client={fixtureClient()}
        connectSessionEvents={events.connect}
      />,
    );
    await attachInitial(events);
    act(() => events.handlers[0].onError({ kind: 'stream_failure' }));
    await waitFor(() => expect(events.connections).toHaveLength(2));
    act(() => events.handlers[1].onError({ kind: 'stream_failure' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('Live updates could not be restored');
    expect(screen.getByRole('button', { name: 'Retry' })).toBeInTheDocument();
    expect(events.connections).toHaveLength(2);
  });
});

describe('live session capacity', () => {
  function capacityError() {
    return new ChaError('session_limit_reached', 'Session limit reached.');
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
    window.history.replaceState(null, '', '/#/s/lobby/planning/');
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
