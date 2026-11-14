import { waitFor, act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, expect, it, vi } from 'vitest';

import type { SessionListing, SessionSnapshot } from '../api/client';
import { ChaProtocolError } from '../api/client';
import { bootstrapFixture, plainVoice, snapshotFixture, voiceOutputRuntimeFixture } from '../test/fixtures';
import { VoiceInputSession } from '../voiceInput';
import { App } from './App';
import { ChaWebError, type ChaWebClient } from './client';
import { unknownSendNotice } from './outcome';

const lobbySessions: SessionListing[] = [
  { id: 'older', label: 'Older', live: false, updated_at: 10 },
  { id: 'planning', label: 'Planning', live: true, updated_at: 50 },
];

const archiveForum = {
  ...bootstrapFixture.forums[1],
  id: 'archive',
  display_name: 'Archive',
};

function boot() {
  return {
    ...bootstrapFixture,
    forums: [...bootstrapFixture.forums, archiveForum],
  };
}

function generation(active: boolean): SessionSnapshot['generation'] {
  return {
    active,
    character_id: active ? 'guide' : '',
    character_display_name: active ? 'Guide' : '',
    phase: active ? 'answering' : 'waiting',
  };
}

function snapshot(
  sessionId: string,
  extras: Partial<SessionSnapshot> = {},
): SessionSnapshot {
  return {
    ...snapshotFixture,
    forum: bootstrapFixture.forums[1],
    session_id: sessionId,
    session_label: sessionId,
    characters: [{
      id: 'guide',
      display_name: 'Guide today',
      appearance: plainVoice,
    }],
    default_character_id: 'guide',
    transcript: [
      {
        id: 1,
        kind: 'human',
        participant_id: 'reader',
        display_name: 'Reader',
        addressed_to: '',
        addressed_to_name: '',
        text: 'First note',
        status: 'complete',
        created_at: 1_700_000_000,
      },
    ],
    generation: generation(false),
    ...extras,
  };
}

function expectSendBlocked(api: ChaWebClient) {
  const creations = vi.mocked(api.createSession).mock.calls.length;
  const submissions = vi.mocked(api.submitInput).mock.calls.length;
  fireEvent.keyDown(screen.getByRole('textbox', { name: 'Message' }), { key: 'Enter' });
  expect(api.createSession).toHaveBeenCalledTimes(creations);
  expect(api.submitInput).toHaveBeenCalledTimes(submissions);
}

function deferred<T>() {
  let resolve: (value: T) => void = () => {};
  let reject: (reason?: unknown) => void = () => {};
  const promise = new Promise<T>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

afterEach(() => {
  cleanup();
  vi.useRealTimers();
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
  Reflect.deleteProperty(document, 'visibilityState');
  Reflect.deleteProperty(window, 'visualViewport');
});

function client(overrides: Partial<ChaWebClient> = {}): ChaWebClient {
  return {
    listNoteItems: vi.fn(async () => []),
    getNote: vi.fn(),
    getVoiceInputRuntime: vi.fn(async () => null),
    connectVoiceInput: vi.fn(),
    startXaiVoiceInput: vi.fn(),
    sendXaiVoiceAudio: vi.fn(),
    stopXaiVoiceInput: vi.fn(),
    cancelXaiVoiceInput: vi.fn(),
    getVoiceOutputRuntime: vi.fn(async () => null),
    startAudio: vi.fn(),
    startAudioBatch: vi.fn(),
    audioUrl: vi.fn(),
    getAudioStatus: vi.fn(async () => ({ cached_entry_ids: [], downloads: [] })),
    getBootstrap: vi.fn(async () => boot()),
    listSessions: vi.fn(async (forumId: string) => (
      forumId === 'archive'
        ? [{ id: 'stored', label: 'Stored', live: false, updated_at: 3 }]
        : lobbySessions
    )),
    createSession: vi.fn(),
    getSession: vi.fn(async (forumId: string, sessionId: string) => snapshot(sessionId, {
      forum: forumId === 'archive' ? archiveForum : bootstrapFixture.forums[1],
    })),
    submitInput: vi.fn(),
    stopSession: vi.fn(),
    deleteSession: vi.fn(),
    ...overrides,
  };
}

async function showList(api = client()) {
  render(<App client={api} />);
  await screen.findByRole('button', { name: 'Forum' });
  return api;
}

function chooseForum(forum: string) {
  fireEvent.click(screen.getByRole('button', { name: 'Forum' }));
  fireEvent.click(screen.getByRole('button', { name: forum === 'lobby' ? 'The Lobby Guide' : 'Archive Guide' }));
}

async function deleteFromList(user: ReturnType<typeof userEvent.setup>, label: string) {
  const remove = await screen.findByRole('button', { name: `Delete session “${label}”` });
  await user.click(remove);
  await user.click(remove);
  return remove;
}

async function openPlanning(api = client()) {
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await screen.findByRole('textbox', { name: 'Message' });
  await screen.findByText('First note');
  return api;
}

const voiceInputRuntime = {
  provider: 'openai' as const, url: 'https://voice.test', model: 'test', delay: 'low' as const,
  prompt: '', send_phrase: 'send',
};

function stubVoiceInput() {
  vi.stubGlobal('isSecureContext', true);
  vi.stubGlobal('Audio', class { src = ''; play = vi.fn(async () => undefined); pause = vi.fn(); });
  vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
  const transport = { cancel: vi.fn(), stop: vi.fn(async () => undefined) };
  const start = vi.spyOn(VoiceInputSession, 'start').mockResolvedValue(transport);
  return { transport, start };
}

it('starts and stops microphone input with the single voice mode toggle', async () => {
  const user = userEvent.setup();
  const { transport, start } = stubVoiceInput();
  await openPlanning(client({
    getVoiceOutputRuntime: vi.fn(async () => voiceOutputRuntimeFixture),
    getVoiceInputRuntime: vi.fn(async () => voiceInputRuntime),
  }));
  const voice = screen.getByRole('button', { name: 'Voice mode' });
  await waitFor(() => expect(voice).toBeEnabled());
  expect(screen.queryByRole('button', { name: /voice input|Send/ })).not.toBeInTheDocument();
  await user.click(voice);
  await screen.findByText('Speak now');
  expect(start).toHaveBeenCalledOnce();
  expect(voice).toHaveAttribute('aria-pressed', 'true');
  expect(screen.getByRole('textbox')).toHaveAttribute('readonly');
  // An empty draft has nothing to send.
  expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
  await user.click(voice);
  await waitFor(() => expect(screen.getByRole('textbox')).not.toHaveAttribute('readonly'));
  expect(transport.stop).toHaveBeenCalledOnce();
  expect(voice).toHaveAttribute('aria-pressed', 'false');
  expect(screen.queryByText('Speak now')).not.toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'Send' })).not.toBeInTheDocument();
});

it('finishes dictation and sends it with the voice mode Send button', async () => {
  const user = userEvent.setup();
  const { transport, start } = stubVoiceInput();
  let hear: (text: string, provisional?: boolean) => void = () => {};
  start.mockImplementation(async (_configuration, onTranscription) => {
    hear = onTranscription;
    return transport;
  });
  const api = await openPlanning(client({
    getVoiceInputRuntime: vi.fn(async () => voiceInputRuntime),
    submitInput: vi.fn(async () => undefined),
  }));
  const voice = screen.getByRole('button', { name: 'Voice mode' });
  await waitFor(() => expect(voice).toBeEnabled());
  await user.click(voice);
  await screen.findByText('Speak now');
  act(() => hear('Book the train', false));
  await user.click(screen.getByRole('button', { name: 'Send' }));
  await waitFor(() => expect(api.submitInput).toHaveBeenCalledWith(
    'lobby', 'planning', expect.stringContaining('Book the train')));
  expect(transport.stop).toHaveBeenCalledOnce();
});

it('uses voice mode for dictation when voice output is not configured', async () => {
  const user = userEvent.setup();
  const { transport, start } = stubVoiceInput();
  await openPlanning(client({ getVoiceInputRuntime: vi.fn(async () => voiceInputRuntime) }));
  const voice = screen.getByRole('button', { name: 'Voice mode' });
  await waitFor(() => expect(voice).toBeEnabled());
  await user.click(voice);
  await screen.findByText('Speak now');
  expect(start).toHaveBeenCalledOnce();
  expect(voice).toHaveAttribute('aria-pressed', 'true');
  await user.click(voice);
  await waitFor(() => expect(voice).toHaveAttribute('aria-pressed', 'false'));
  expect(transport.stop).toHaveBeenCalledOnce();
});

it('turns voice mode off when a hidden page stops the microphone and restarts it with one press', async () => {
  const user = userEvent.setup();
  let visible = true;
  Object.defineProperty(document, 'visibilityState', {
    configurable: true,
    get: () => (visible ? 'visible' : 'hidden'),
  });
  const { start } = stubVoiceInput();
  await openPlanning(client({
    getVoiceOutputRuntime: vi.fn(async () => voiceOutputRuntimeFixture),
    getVoiceInputRuntime: vi.fn(async () => voiceInputRuntime),
  }));
  const voice = screen.getByRole('button', { name: 'Voice mode' });
  await waitFor(() => expect(voice).toBeEnabled());
  await user.click(voice);
  await screen.findByText('Speak now');
  visible = false;
  act(() => { document.dispatchEvent(new Event('visibilitychange')); });
  await waitFor(() => expect(voice).toHaveAttribute('aria-pressed', 'false'));
  visible = true;
  act(() => { document.dispatchEvent(new Event('visibilitychange')); });
  await user.click(voice);
  await screen.findByText('Speak now');
  expect(start).toHaveBeenCalledTimes(2);
  expect(voice).toHaveAttribute('aria-pressed', 'true');
});

it('stops the microphone when automatic audio stops after an error', async () => {
  const user = userEvent.setup();
  stubVoiceInput();
  const reply = (status: 'streaming' | 'complete') => ({
    id: 2, kind: 'character' as const, participant_id: 'guide', display_name: 'Guide',
    addressed_to: '', addressed_to_name: '', text: 'Later reply', status, created_at: 1_700_000_100,
  });
  const first = snapshot('planning', { generation: generation(true) });
  const api = client({
    getVoiceOutputRuntime: vi.fn(async () => voiceOutputRuntimeFixture),
    getVoiceInputRuntime: vi.fn(async () => voiceInputRuntime),
    getSession: vi.fn()
      .mockResolvedValueOnce({ ...first, transcript: [...first.transcript, reply('streaming')] })
      .mockResolvedValue(snapshot('planning', {
        transcript: [...first.transcript, reply('complete')],
      })),
    startAudioBatch: vi.fn(async () => { throw new ChaWebError(500, 'No audio.', 'internal_error'); }),
  });
  await openPlanning(api);
  const voice = screen.getByRole('button', { name: 'Voice mode' });
  await waitFor(() => expect(voice).toBeEnabled());
  await user.click(voice);
  expect(voice).toHaveAttribute('aria-pressed', 'true');
  await waitFor(() => expect(api.startAudioBatch).toHaveBeenCalled(), { timeout: 3000 });
  await waitFor(() => expect(voice).toHaveAttribute('aria-pressed', 'false'));
  expect(screen.getByText('No audio.')).toBeInTheDocument();
  expect(screen.queryByText(/Speak now|Microphone/)).not.toBeInTheDocument();
});

it('pauses conversation reads in Notes without stopping server generation and recovers on Back', async () => {
  vi.useFakeTimers();
  const api = client({ getSession: vi.fn(async () => snapshot('planning', { generation: generation(true) })) });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(async () => { await vi.advanceTimersByTimeAsync(0); });
  expect(screen.getByText('First note')).toBeInTheDocument();
  fireEvent.click(screen.getByRole('link', { name: 'Notes' }));
  await act(async () => { await vi.advanceTimersByTimeAsync(0); });
  const reads = vi.mocked(api.getSession).mock.calls.length;
  await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
  expect(api.getSession).toHaveBeenCalledTimes(reads);
  expect(api.stopSession).not.toHaveBeenCalled();
  expect(screen.getByText('No notes.')).toBeInTheDocument();
  act(() => {
    window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
    window.dispatchEvent(new PopStateEvent('popstate'));
  });
  await act(async () => { await vi.advanceTimersByTimeAsync(0); });
  expect(api.getSession).toHaveBeenCalledTimes(reads + 1);
  expect(screen.getByText('First note')).toBeInTheDocument();
});

it('deletes from the root without confirmation and preserves unrelated drafts', async () => {
  const user = userEvent.setup();
  const deleted = deferred<void>();
  let removed = false;
  const api = await showList(client({
    deleteSession: vi.fn(() => deleted.promise.then(() => { removed = true; })),
    listSessions: vi.fn(async () => removed ? [lobbySessions[0]!] : lobbySessions),
  }));
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  await user.type(screen.getByRole('textbox'), 'Separate new draft');
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  await user.click(await screen.findByRole('button', { name: /^Planning/ }));
  await screen.findByText('First note');
  await user.type(screen.getByRole('textbox'), 'Deleted draft');
  expect(screen.queryByRole('button', { name: /Delete session/ })).not.toBeInTheDocument();
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  const remove = await deleteFromList(user, 'Planning');
  expect(api.deleteSession).toHaveBeenCalledExactlyOnceWith('lobby', 'planning');
  expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  expect(remove).toBeDisabled();
  await act(async () => deleted.resolve());
  await waitFor(() => expect(screen.queryByRole('button', { name: /^Planning/ })).not.toBeInTheDocument());
  expect(window.location.hash).toBe('');
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  expect(screen.getByRole('textbox')).toHaveValue('Separate new draft');
  await settleHistory(() => {
    window.history.pushState(null, '', '/#/forums/lobby/sessions/planning');
    window.dispatchEvent(new PopStateEvent('popstate'));
  });
  await screen.findByText('First note');
  expect(screen.getByRole('textbox')).toHaveValue('');
});

it('keeps a failed deletion and its draft and permits an explicit retry', async () => {
  const user = userEvent.setup();
  const api = await openPlanning(client({
    deleteSession: vi.fn(async () => { throw new ChaWebError(500, 'Try again.', 'session_stopping'); }),
  }));
  await user.type(screen.getByRole('textbox'), 'Keep this');
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  await deleteFromList(user, 'Planning');
  expect(await screen.findByRole('alert')).toHaveTextContent('Try again.');
  expect(screen.getByRole('button', { name: 'Delete session “Planning”' })).toBeEnabled();
  // Delete is still shown after the failure, so one click retries.
  await user.click(await screen.findByRole('button', { name: 'Delete session “Planning”' }));
  expect(api.deleteSession).toHaveBeenCalledTimes(2);
  // The first tap closes the shown Delete. The second tap opens the session.
  await user.click(screen.getByRole('button', { name: /^Planning/ }));
  expect(screen.queryByRole('textbox')).not.toBeInTheDocument();
  await user.click(screen.getByRole('button', { name: /^Planning/ }));
  expect(screen.getByRole('textbox')).toHaveValue('Keep this');
  await screen.findByText('First note');
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
});

it('offers deletion after an accepted Stop before a snapshot shows the idle session', async () => {
  const user = userEvent.setup();
  const api = await openPlanning(client({
    getSession: vi.fn(async () => snapshot('planning', { generation: generation(true) })),
    stopSession: vi.fn(async () => undefined),
  }));
  await user.click(await screen.findByRole('button', { name: 'Stop' }));
  await waitFor(() => expect(api.stopSession).toHaveBeenCalledOnce());
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  expect(await screen.findByRole('button', { name: 'Delete session “Planning”' })).toBeEnabled();
});

it('prevents deletion while input or Stop is pending', async () => {
  const user = userEvent.setup();
  const api = await openPlanning(client({
    submitInput: vi.fn(() => new Promise<void>(() => {})),
    stopSession: vi.fn(() => new Promise<void>(() => {})),
  }));
  await user.type(screen.getByRole('textbox'), 'Hello');
  await user.click(screen.getByRole('textbox', { name: 'Message' }));
  await user.keyboard('{Enter}');
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  await screen.findByRole('button', { name: /^Planning/ });
  expect(screen.queryByRole('button', { name: 'Delete session “Planning”' })).not.toBeInTheDocument();
  await user.click(screen.getByRole('button', { name: /^Planning/ }));
  await screen.findByRole('button', { name: 'Stop' });
  await user.click(screen.getByRole('button', { name: 'Stop' }));
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  await screen.findByRole('button', { name: /^Planning/ });
  expect(screen.queryByRole('button', { name: 'Delete session “Planning”' })).not.toBeInTheDocument();
  expect(api.deleteSession).not.toHaveBeenCalled();
});

it('does not leave another conversation when a root deletion finishes', async () => {
  const user = userEvent.setup();
  const deleted = deferred<void>();
  await showList(client({ deleteSession: vi.fn(() => deleted.promise) }));
  await deleteFromList(user, 'Planning');
  await user.click(screen.getByRole('button', { name: /^Older/ }));
  await screen.findByText('First note');
  await user.type(screen.getByRole('textbox'), 'Other session draft');
  await act(async () => deleted.resolve());
  expect(screen.getByRole('textbox')).toHaveValue('Other session draft');
  expect(window.location.hash).toContain('/older');
});

it('treats a session already deleted elsewhere as deleted', async () => {
  const user = userEvent.setup();
  await showList(client({
    listSessions: vi.fn().mockResolvedValueOnce(lobbySessions).mockResolvedValue([lobbySessions[0]!]),
    deleteSession: vi.fn(async () => { throw new ChaWebError(404, 'Not found.', 'not_found'); }),
  }));
  await deleteFromList(user, 'Planning');
  await waitFor(() => expect(screen.queryByRole('button', { name: /^Planning/ })).not.toBeInTheDocument());
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
});

it('keeps one new draft per forum and does not create a session', async () => {
  const user = userEvent.setup();
  const api = await showList();
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  expect(window.location.hash).toBe('');
  await user.type(screen.getByRole('textbox', { name: 'Message' }), 'Lobby draft');
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  await user.click(screen.getByRole('button', { name: 'Forum' }));
  await user.click(screen.getByRole('button', { name: /^Archive / }));
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  const archive = screen.getByRole('textbox', { name: 'Message' });
  expect(archive).toHaveValue('');
  await user.type(archive, 'Archive draft');
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  await user.click(screen.getByRole('button', { name: 'Forum' }));
  await user.click(screen.getByRole('button', { name: /^The Lobby / }));
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('Lobby draft');
  expect(api.createSession).not.toHaveBeenCalled();
  expect(api.getSession).not.toHaveBeenCalled();
  expect(api.submitInput).not.toHaveBeenCalled();
});

it('clears only the submitted revision and keeps a rejected draft', async () => {
  const user = userEvent.setup();
  const input = deferred<void>();
  const afterAck = deferred<SessionSnapshot>();
  let reads = 0;
  const api = await openPlanning(client({
    submitInput: vi.fn(() => input.promise),
    getSession: vi.fn(() => {
      reads += 1;
      return reads === 1 ? Promise.resolve(snapshot('planning')) : afterAck.promise;
    }),
  }));
  const box = screen.getByRole('textbox', { name: 'Message' });
  await user.type(box, 'Hello');
  await user.click(screen.getByRole('textbox', { name: 'Message' }));
  await user.keyboard('{Enter}');
  fireEvent.change(box, { target: { value: 'Hello!' } });
  await act(async () => {
    input.resolve();
  });
  expect(box).toHaveValue('Hello!');
  expect(api.submitInput).toHaveBeenCalledTimes(1);
  expect(api.submitInput).toHaveBeenCalledWith('lobby', 'planning', 'Hello');
  expect(screen.getByRole('status')).toHaveTextContent('Hello');
  expectSendBlocked(api);
  await act(async () => afterAck.resolve(snapshot('planning')));

  api.submitInput = vi.fn(async () => {
    throw new ChaWebError(422, 'Unknown command', 'invalid_argument');
  });
  await user.clear(box);
  await user.type(box, '@- note');
  await user.click(screen.getByRole('textbox', { name: 'Message' }));
  await user.keyboard('{Enter}');
  expect(screen.getByRole('alert')).toHaveTextContent('Unknown command');
  expect(box).toHaveValue('@- note');
  await user.keyboard('{Enter}');
  expect(api.submitInput).toHaveBeenCalledTimes(2);
});

async function settleHistory(move: () => void) {
  await act(async () => {
    move();
    // jsdom traverses history across two queued tasks.
    await new Promise((resolve) => {
      setTimeout(resolve, 0);
    });
    await new Promise((resolve) => {
      setTimeout(resolve, 0);
    });
  });
}

it('follows Back and Forward without a second request for one place', async () => {
  const user = userEvent.setup();
  const api = await showList();
  await user.click(await screen.findByRole('button', { name: /^Planning/ }));
  await screen.findByText('First note');
  expect(api.getSession).toHaveBeenCalledTimes(1);
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  await user.click(await screen.findByRole('button', { name: /^Older/ }));
  await screen.findByText('First note');
  expect(api.getSession).toHaveBeenCalledTimes(2);
  expect(api.getSession).toHaveBeenLastCalledWith('lobby', 'older');

  await settleHistory(() => window.history.back());
  expect(screen.getByRole('button', { name: 'Forum' })).toBeInTheDocument();
  expect(api.getSession).toHaveBeenCalledTimes(2);
  await settleHistory(() => window.history.back());
  expect(api.getSession).toHaveBeenCalledTimes(3);
  expect(api.getSession).toHaveBeenLastCalledWith('lobby', 'planning');
  await settleHistory(() => window.history.forward());
  expect(screen.getByRole('button', { name: 'Forum' })).toBeInTheDocument();
  expect(api.getSession).toHaveBeenCalledTimes(3);
  await settleHistory(() => window.history.forward());
  expect(api.getSession).toHaveBeenCalledTimes(4);
  expect(api.getSession).toHaveBeenLastCalledWith('lobby', 'older');
});

it('associates a late creation with its draft and leaves the other view', async () => {
  const user = userEvent.setup();
  const created = deferred<{ id: string; label: string }>();
  let rows = lobbySessions;
  const api = await showList(client({
    createSession: vi.fn(() => created.promise),
    listSessions: vi.fn(async (forumId: string) => (
      forumId === 'archive'
        ? [{ id: 'stored', label: 'Stored', live: false, updated_at: 3 }]
        : rows
    )),
  }));
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  const box = screen.getByRole('textbox', { name: 'Message' });
  await user.type(box, '@Guide hello');
  await user.click(screen.getByRole('textbox', { name: 'Message' }));
  await user.keyboard('{Enter}');
  fireEvent.change(box, { target: { value: '@Guide hello!' } });
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  await user.click(screen.getByRole('button', { name: 'Forum' }));
  await user.click(screen.getByRole('button', { name: /^Archive / }));
  await user.click(screen.getByRole('button', { name: /^Stored/ }));
  await screen.findByText('First note');
  const archiveBox = screen.getByRole('textbox', { name: 'Message' });
  await user.type(archiveBox, 'Archive text');
  await act(async () => {
    rows = [
      { id: 'fresh', label: 'Fresh', live: false, updated_at: 90 },
      ...lobbySessions,
    ];
    created.resolve({ id: 'fresh', label: 'Fresh' });
  });
  expect(window.location.hash).toContain('/sessions/stored');
  expect(archiveBox).toHaveValue('Archive text');
  expect(api.createSession).toHaveBeenCalledTimes(1);
  expect(api.createSession).toHaveBeenCalledWith('lobby', '@Guide hello');
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  await user.click(screen.getByRole('button', { name: 'Forum' }));
  await user.click(screen.getByRole('button', { name: /^The Lobby / }));
  await user.click(await screen.findByRole('button', { name: /^Fresh/ }));
  expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('@Guide hello!');
});

it('keeps a pending input when the reader leaves and returns', async () => {
  const user = userEvent.setup();
  const input = deferred<void>();
  const afterAck = deferred<SessionSnapshot>();
  let reads = 0;
  const api = await openPlanning(client({
    submitInput: vi.fn(() => input.promise),
    getSession: vi.fn(() => {
      reads += 1;
      return reads === 1 ? Promise.resolve(snapshot('planning')) : afterAck.promise;
    }),
  }));
  const box = screen.getByRole('textbox', { name: 'Message' });
  await user.type(box, 'Still sending');
  await user.click(screen.getByRole('textbox', { name: 'Message' }));
  await user.keyboard('{Enter}');
  await user.click(screen.getByRole('link', { name: 'Chats' }));
  await user.click(screen.getByRole('button', { name: /^Planning/ }));
  expect(await screen.findByRole('status')).toHaveTextContent('Sending');
  expect(api.submitInput).toHaveBeenCalledTimes(1);
  expect(screen.getByRole('button', { name: 'Stop' })).toBeEnabled();
  await act(async () => {
    input.resolve();
  });
  expect(screen.getByRole('status')).toHaveTextContent('Still sending');
});

it('disables Stop until the first session id, then stops without waiting for input', async () => {
  const user = userEvent.setup();
  const created = deferred<{ id: string; label: string }>();
  const firstRead = deferred<SessionSnapshot>();
  const laterRead = deferred<SessionSnapshot>();
  const input = deferred<void>();
  const stop = deferred<void>();
  let reads = 0;
  const api = await showList(client({
    createSession: vi.fn(() => created.promise),
    getSession: vi.fn(() => {
      reads += 1;
      return reads === 1 ? firstRead.promise : laterRead.promise;
    }),
    submitInput: vi.fn(() => input.promise),
    stopSession: vi.fn(() => stop.promise),
  }));
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  await user.type(screen.getByRole('textbox', { name: 'Message' }), 'First turn');
  await user.click(screen.getByRole('textbox', { name: 'Message' }));
  await user.keyboard('{Enter}');
  expectSendBlocked(api);
  expect(screen.queryByRole('button', { name: 'Stop' })).not.toBeInTheDocument();
  expect(api.stopSession).not.toHaveBeenCalled();
  await act(async () => {
    created.resolve({ id: 'fresh', label: 'Fresh' });
  });
  expect(window.location.hash).toBe('#/forums/lobby/sessions/fresh');
  expect(screen.getByRole('button', { name: 'Stop' })).toBeEnabled();
  await act(async () => {
    firstRead.resolve(snapshot('fresh'));
  });
  const box = screen.getByRole('textbox', { name: 'Message' });
  await user.type(box, 'Next');
  await user.click(screen.getByRole('textbox', { name: 'Message' }));
  await user.keyboard('{Enter}');
  await user.click(screen.getByRole('button', { name: 'Stop' }));
  expect(api.stopSession).toHaveBeenCalledTimes(1);
  expect(api.stopSession).toHaveBeenCalledWith('lobby', 'fresh');
  expect(input.promise).toBe(input.promise);
  expect(screen.getByRole('alert')).toHaveTextContent('Stop requested');
  expect(screen.getByRole('button', { name: 'Stop' })).toBeDisabled();
  await act(async () => {
    stop.resolve();
  });
  expect(screen.getByRole('button', { name: 'Stop' })).toBeEnabled();
  await act(async () => {
    laterRead.resolve(snapshot('fresh', { generation: generation(true) }));
  });
  expect(screen.getByRole('button', { name: 'Stop' })).toBeEnabled();
  await act(async () => {
    input.resolve();
  });
  expect(api.submitInput).toHaveBeenCalledTimes(1);
});

it('shows a self-note as finished when generation is inactive', async () => {
  vi.useFakeTimers();
  const reads: SessionSnapshot[] = [
    snapshot('planning'),
    snapshot('planning', {
      transcript: [{
        id: 4,
        kind: 'human',
        participant_id: 'reader',
        display_name: 'Reader',
        addressed_to: '',
        addressed_to_name: '',
        text: 'Note to self',
        status: 'complete',
        created_at: 5,
      }],
      generation: generation(false),
    }),
  ];
  const api = client({
    getSession: vi.fn(async () => reads.shift() ?? snapshot('planning')),
    submitInput: vi.fn(async () => undefined),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  const box = screen.getByRole('textbox', { name: 'Message' }) as HTMLTextAreaElement;
  fireEvent.change(box, { target: { value: 'Note to self' } });
  fireEvent.keyDown(screen.getByRole('textbox', { name: 'Message' }), { key: 'Enter' });
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.getByText('Note to self')).toBeInTheDocument();
  expect(api.submitInput).toHaveBeenCalledTimes(1);
  const calls = vi.mocked(api.getSession).mock.calls.length;
  await act(() => vi.advanceTimersByTimeAsync(5_000));
  expect(api.getSession).toHaveBeenCalledTimes(calls);
  expect(api.submitInput).toHaveBeenCalledTimes(1);
});

it('polls once, one second after completion, and coalesces a Stop acknowledgement', async () => {
  vi.useFakeTimers();
  const gates = [deferred<SessionSnapshot>(), deferred<SessionSnapshot>(), deferred<SessionSnapshot>()];
  let reads = 0;
  const api = client({
    getSession: vi.fn(() => {
      const gate = gates[reads] ?? deferred<SessionSnapshot>();
      reads += 1;
      return gate.promise;
    }),
    stopSession: vi.fn(async () => undefined),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(api.getSession).toHaveBeenCalledTimes(1);
  await act(() => vi.advanceTimersByTimeAsync(5_000));
  expect(api.getSession).toHaveBeenCalledTimes(1);
  await act(async () => {
    gates[0].resolve(snapshot('planning', { generation: generation(true) }));
    await vi.advanceTimersByTimeAsync(999);
  });
  expect(api.getSession).toHaveBeenCalledTimes(1);
  await act(() => vi.advanceTimersByTimeAsync(1));
  expect(api.getSession).toHaveBeenCalledTimes(2);
  fireEvent.click(screen.getByRole('button', { name: 'Stop' }));
  expect(api.stopSession).toHaveBeenCalledTimes(1);
  expect(api.getSession).toHaveBeenCalledTimes(2);
  await act(async () => {
    gates[1].resolve(snapshot('planning', { generation: generation(true) }));
    await vi.advanceTimersByTimeAsync(0);
  });
  expect(api.getSession).toHaveBeenCalledTimes(3);
  await act(async () => {
    gates[2].resolve(snapshot('planning', { generation: generation(false) }));
    await vi.advanceTimersByTimeAsync(5_000);
  });
  expect(api.getSession).toHaveBeenCalledTimes(3);
  expectSendBlocked(api);
});

it('replaces a Stop failure notice when a fresh snapshot shows the session idle', async () => {
  vi.useFakeTimers();
  const api = client({
    getSession: vi.fn()
      .mockResolvedValueOnce(snapshot('planning', { generation: generation(true) }))
      .mockResolvedValueOnce(snapshot('planning', { generation: generation(true) }))
      .mockResolvedValue(snapshot('planning', { notice: 'Reply stopped.' })),
    stopSession: vi.fn(async () => {
      throw new ChaWebError(0, 'The request timed out.');
    }),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.click(screen.getByRole('button', { name: 'Stop' }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.getByRole('alert')).toHaveTextContent('The request timed out.');
  await act(() => vi.advanceTimersByTimeAsync(1_000));
  expect(screen.getByRole('alert')).toHaveTextContent('Reply stopped.');
  expect(api.stopSession).toHaveBeenCalledTimes(1);
});

it('polls an idle open chat after thirty seconds and pauses while hidden', async () => {
  vi.useFakeTimers();
  let visible = true;
  Object.defineProperty(document, 'visibilityState', {
    configurable: true,
    get: () => (visible ? 'visible' : 'hidden'),
  });
  const first = snapshot('planning', { generation: generation(false) });
  const second = snapshot('planning', {
    generation: generation(false),
    transcript: [
      ...snapshot('planning').transcript,
      {
        id: 2,
        kind: 'human',
        participant_id: 'reader',
        display_name: 'Reader',
        addressed_to: '',
        addressed_to_name: '',
        text: 'Synced elsewhere',
        status: 'complete',
        created_at: 1_700_000_100,
      },
    ],
  });
  const api = client({
    getSession: vi.fn()
      .mockResolvedValueOnce(first)
      .mockResolvedValueOnce(first)
      .mockResolvedValue(second),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(api.getSession).toHaveBeenCalledTimes(1);
  // The one-second active cadence does not apply while the reply is idle.
  await act(() => vi.advanceTimersByTimeAsync(5_000));
  expect(api.getSession).toHaveBeenCalledTimes(1);
  visible = false;
  fireEvent(document, new Event('visibilitychange'));
  await act(() => vi.advanceTimersByTimeAsync(60_000));
  expect(api.getSession).toHaveBeenCalledTimes(1);
  visible = true;
  fireEvent(document, new Event('visibilitychange'));
  await act(() => vi.advanceTimersByTimeAsync(0));
  // Returning to the page reads immediately, then the idle cadence resumes.
  expect(api.getSession).toHaveBeenCalledTimes(2);
  await act(() => vi.advanceTimersByTimeAsync(29_999));
  expect(api.getSession).toHaveBeenCalledTimes(2);
  await act(() => vi.advanceTimersByTimeAsync(1));
  expect(api.getSession).toHaveBeenCalledTimes(3);
  expect(screen.getByText('Synced elsewhere')).toBeInTheDocument();
});

it('retries a reserved chat read and shows the cancelled reply and incoming entry', async () => {
  vi.useFakeTimers();
  const pending = {
    ...snapshot('planning').transcript[0],
    id: 2, kind: 'character' as const, participant_id: 'guide', text: 'Partial reply',
    status: 'streaming' as const, request_id: 1,
  };
  const active = snapshot('planning', {
    generation: generation(true),
    transcript: [...snapshot('planning').transcript, pending],
  });
  const refreshed = snapshot('planning', {
    transcript: [
      active.transcript[0],
      { ...pending, status: 'cancelled' },
      { ...pending, id: 3, status: 'complete', text: 'Synced reply' },
    ],
  });
  const api = client({
    getSession: vi.fn()
      .mockResolvedValueOnce(active)
      .mockRejectedValueOnce(new ChaWebError(500, 'The session is stopping.', 'session_stopping'))
      .mockResolvedValue(refreshed),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.getByRole('button', { name: 'Stop' })).toBeInTheDocument();
  await act(() => vi.advanceTimersByTimeAsync(999));
  expect(api.getSession).toHaveBeenCalledTimes(1);
  await act(() => vi.advanceTimersByTimeAsync(1));
  expect(screen.getByRole('alert')).toHaveTextContent('Reconnecting');
  await act(() => vi.advanceTimersByTimeAsync(1_000));
  expect(screen.getByText('Synced reply')).toBeInTheDocument();
  expect(screen.getByText('Stopped')).toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'Stop' })).not.toBeInTheDocument();
  expect(api.getSession).toHaveBeenCalledTimes(4);
  await act(() => vi.advanceTimersByTimeAsync(29_999));
  expect(api.getSession).toHaveBeenCalledTimes(4);
  await act(() => vi.advanceTimersByTimeAsync(1));
  expect(api.getSession).toHaveBeenCalledTimes(5);
});

it('does not poll a hidden page, the session list, or a new draft', async () => {
  vi.useFakeTimers();
  let visible = true;
  Object.defineProperty(document, 'visibilityState', {
    configurable: true,
    get: () => (visible ? 'visible' : 'hidden'),
  });
  const gates = [deferred<SessionSnapshot>(), deferred<SessionSnapshot>()];
  let reads = 0;
  const api = client({
    getSession: vi.fn(() => {
      const gate = gates[reads] ?? deferred<SessionSnapshot>();
      reads += 1;
      return gate.promise;
    }),
  });
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.click(screen.getByRole('button', { name: 'New Session' }));
  await act(() => vi.advanceTimersByTimeAsync(3_000));
  expect(api.getSession).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole('link', { name: 'Chats' }));
  fireEvent.click(screen.getByRole('button', { name: /^Planning/ }));
  await act(async () => {
    gates[0].resolve(snapshot('planning', { generation: generation(true) }));
    await vi.advanceTimersByTimeAsync(0);
  });
  expect(api.getSession).toHaveBeenCalledTimes(1);
  visible = false;
  fireEvent(document, new Event('visibilitychange'));
  await act(() => vi.advanceTimersByTimeAsync(5_000));
  expect(api.getSession).toHaveBeenCalledTimes(1);
  visible = true;
  fireEvent(document, new Event('visibilitychange'));
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(api.getSession).toHaveBeenCalledTimes(2);
  fireEvent.click(screen.getByRole('link', { name: 'Chats' }));
  await act(() => vi.advanceTimersByTimeAsync(5_000));
  expect(api.getSession).toHaveBeenCalledTimes(2);
});

it('refreshes the list when a title changes or generation finishes, then stops', async () => {
  vi.useFakeTimers();
  const gates = [deferred<SessionSnapshot>(), deferred<SessionSnapshot>()];
  let reads = 0;
  const api = client({
    getSession: vi.fn(() => {
      const gate = gates[reads] ?? deferred<SessionSnapshot>();
      reads += 1;
      return gate.promise;
    }),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(api.listSessions).not.toHaveBeenCalled();
  await act(async () => {
    gates[0].resolve(snapshot('planning', {
      session_label: 'Temp',
      generation: generation(true),
    }));
    await vi.advanceTimersByTimeAsync(1_000);
  });
  expect(api.getSession).toHaveBeenCalledTimes(2);
  expect(api.listSessions).not.toHaveBeenCalled();
  await act(async () => {
    gates[1].resolve(snapshot('planning', {
      session_label: 'Real title',
      generation: generation(false),
    }));
    await vi.advanceTimersByTimeAsync(0);
  });
  expect(api.listSessions).toHaveBeenCalledWith('lobby');
  const lists = vi.mocked(api.listSessions).mock.calls.length;
  await act(() => vi.advanceTimersByTimeAsync(5_000));
  expect(api.getSession).toHaveBeenCalledTimes(2);
  expect(api.listSessions).toHaveBeenCalledTimes(lists);
});

it('retries a hidden read on return and stops after malformed data', async () => {
  vi.useFakeTimers();
  let visible = true;
  Object.defineProperty(document, 'visibilityState', {
    configurable: true,
    get: () => (visible ? 'visible' : 'hidden'),
  });
  const api = client({
    getSession: vi.fn()
      .mockRejectedValueOnce(new ChaWebError(503, 'The request failed.'))
      .mockResolvedValueOnce(snapshot('planning', { generation: generation(true) }))
      .mockResolvedValueOnce(snapshot('planning', { generation: generation(true) }))
      .mockResolvedValueOnce(snapshot('other')),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.getByRole('alert')).toHaveTextContent('Reconnecting');
  visible = false;
  fireEvent(document, new Event('visibilitychange'));
  await act(() => vi.advanceTimersByTimeAsync(10_000));
  expect(api.getSession).toHaveBeenCalledTimes(1);
  visible = true;
  fireEvent(document, new Event('visibilitychange'));
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(api.getBootstrap).toHaveBeenCalledTimes(2);
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  expect(vi.mocked(api.getSession).mock.calls.length).toBeGreaterThanOrEqual(2);
  await act(() => vi.advanceTimersByTimeAsync(5_000));
  expect(screen.getByRole('alert')).toHaveTextContent('incompatible response');
  expect(screen.getByRole('button', { name: 'Retry' })).toBeInTheDocument();
  const calls = vi.mocked(api.getSession).mock.calls.length;
  await act(() => vi.advanceTimersByTimeAsync(20_000));
  expect(api.getSession).toHaveBeenCalledTimes(calls);
});

it('backs off 1, 2, 4, then 10 seconds and ends with a Retry button', async () => {
  vi.useFakeTimers();
  const api = client({
    getSession: vi.fn(async () => {
      throw new ChaWebError(503, 'The request failed.');
    }),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  const reads = () => vi.mocked(api.getSession).mock.calls.length;
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(reads()).toBe(1);
  for (const [wait, total] of [[1_000, 2], [2_000, 3], [4_000, 4], [10_000, 5]]) {
    await act(() => vi.advanceTimersByTimeAsync(wait - 1));
    expect(reads()).toBe(total - 1);
    await act(() => vi.advanceTimersByTimeAsync(1));
    expect(reads()).toBe(total);
  }
  await act(() => vi.advanceTimersByTimeAsync(60_000));
  const final = reads();
  expect(screen.getByRole('button', { name: 'Retry' })).toBeInTheDocument();
  await act(() => vi.advanceTimersByTimeAsync(60_000));
  expect(reads()).toBe(final);
});

it('stops polling a missing session and refreshes the list', async () => {
  vi.useFakeTimers();
  const api = client({
    getSession: vi.fn(async () => {
      throw new ChaWebError(404, 'Session not found.', 'not_found');
    }),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  const lists = vi.mocked(api.listSessions).mock.calls.length;
  expect(lists).toBeGreaterThanOrEqual(1);
  await act(() => vi.advanceTimersByTimeAsync(30_000));
  expect(api.getSession).toHaveBeenCalledTimes(1);
});

it('does not replay a lost first send when the list has no new row', async () => {
  vi.useFakeTimers();
  const api = client({
    createSession: vi.fn(async () => {
      throw new ChaWebError(0, 'The request timed out.');
    }),
  });
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.click(screen.getByRole('button', { name: 'New Session' }));
  const box = screen.getByRole('textbox', { name: 'Message' }) as HTMLTextAreaElement;
  fireEvent.change(box, { target: { value: 'Maybe sent' } });
  fireEvent.keyDown(screen.getByRole('textbox', { name: 'Message' }), { key: 'Enter' });
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.getByRole('alert')).toHaveTextContent(unknownSendNotice);
  expect(box).toHaveValue('Maybe sent');
  expectSendBlocked(api);
  await act(() => vi.advanceTimersByTimeAsync(30_000));
  expect(api.createSession).toHaveBeenCalledTimes(1);
  expect(screen.queryByRole('button', { name: /Maybe sent/ })).not.toBeInTheDocument();
  fireEvent.click(screen.getByRole('link', { name: 'Chats' }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.queryByRole('button', { name: /fresh/i })).not.toBeInTheDocument();
  fireEvent.click(screen.getByRole('button', { name: 'New Session' }));
  expect(screen.getByRole('alert')).toHaveTextContent(unknownSendNotice);
  expect(api.createSession).toHaveBeenCalledTimes(1);
  fireEvent.keyDown(screen.getByRole('textbox', { name: 'Message' }), { key: 'Enter' });
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(api.createSession).toHaveBeenCalledTimes(2);
});

it('does not replay a command timeout and keeps the draft', async () => {
  vi.useFakeTimers();
  const api = client({
    submitInput: vi.fn(async () => {
      throw new ChaWebError(500, 'The command outcome is unknown.', 'command_timeout');
    }),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  const box = screen.getByRole('textbox', { name: 'Message' }) as HTMLTextAreaElement;
  fireEvent.change(box, { target: { value: 'Check first' } });
  fireEvent.keyDown(screen.getByRole('textbox', { name: 'Message' }), { key: 'Enter' });
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.getByRole('alert')).toHaveTextContent(unknownSendNotice);
  expect(box).toHaveValue('Check first');
  await act(() => vi.advanceTimersByTimeAsync(30_000));
  expect(api.submitInput).toHaveBeenCalledTimes(1);
  expect(screen.getByText('First note')).toBeInTheDocument();
});

it('keeps an oversized prompt and shows the server message', async () => {
  const user = userEvent.setup();
  const api = await showList(client({
    createSession: vi.fn(async () => {
      throw new ChaWebError(400, 'Prompt is too large.', 'prompt_too_large');
    }),
  }));
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  const box = screen.getByRole('textbox', { name: 'Message' });
  await user.type(box, 'Too big');
  await user.click(screen.getByRole('textbox', { name: 'Message' }));
  await user.keyboard('{Enter}');
  expect(screen.getByRole('alert')).toHaveTextContent('Prompt is too large.');
  expect(box).toHaveValue('Too big');
  expect(window.location.hash).toBe('');
  expect(api.createSession).toHaveBeenCalledTimes(1);
  expect(api.getSession).not.toHaveBeenCalled();
});

it('keeps the editor, caret, size, and reading position across snapshots', async () => {
  const user = userEvent.setup();
  Object.defineProperty(window, 'visualViewport', {
    configurable: true,
    value: {
      height: 700,
      offsetTop: 0,
      addEventListener() {},
      removeEventListener() {},
    },
  });
  let visible = true;
  Object.defineProperty(document, 'visibilityState', {
    configurable: true,
    get: () => (visible ? 'visible' : 'hidden'),
  });
  const reads = [
    snapshot('planning'),
    snapshot('planning', {
      transcript: [
        {
          id: 1,
          kind: 'human',
          participant_id: 'reader',
          display_name: 'Reader',
          addressed_to: '',
          addressed_to_name: '',
          text: 'First note',
          status: 'complete',
          created_at: 1,
        },
        {
          id: 9,
          kind: 'character',
          participant_id: 'guide',
          display_name: 'Guide',
          addressed_to: '',
          addressed_to_name: '',
          text: 'Later reply',
          status: 'complete',
          created_at: 2,
        },
      ],
    }),
  ];
  const api = client({
    getSession: vi.fn(async () => reads.shift() ?? snapshot('planning')),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  const box = await screen.findByRole('textbox', { name: 'Message' }) as HTMLTextAreaElement;
  await screen.findByText('First note');
  await user.click(screen.getByRole('button', { name: 'Expand editor' }));
  expect(box).toHaveStyle({ height: '306px' });
  await user.type(box, 'Dictated');
  box.setSelectionRange(3, 3);
  const scroller = screen.getByLabelText('Conversation transcript');
  Object.defineProperty(scroller, 'scrollHeight', { configurable: true, get: () => 1000 });
  Object.defineProperty(scroller, 'clientHeight', { configurable: true, get: () => 100 });
  scroller.scrollTop = 0;
  fireEvent.scroll(scroller);
  visible = false;
  fireEvent(document, new Event('visibilitychange'));
  visible = true;
  fireEvent(document, new Event('visibilitychange'));
  expect(await screen.findByText('Later reply')).toBeInTheDocument();
  expect(screen.getByRole('textbox', { name: 'Message' })).toBe(box);
  expect(box).toHaveValue('Dictated');
  expect(box.selectionStart).toBe(3);
  expect(box).toHaveStyle({ height: '306px' });
  expect(scroller.scrollTop).toBe(0);
  expect(api.submitInput).not.toHaveBeenCalled();
});

it('keeps first Sends pending independently across forums', async () => {
  vi.useFakeTimers();
  const pending = deferred<{ id: string; label: string }>();
  const api = client({ createSession: vi.fn(() => pending.promise) });
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  for (const forum of ['lobby', 'archive']) {
    chooseForum(forum);
    fireEvent.click(screen.getByRole('button', { name: 'New Session' }));
    fireEvent.change(screen.getByRole('textbox'), { target: { value: forum } });
    fireEvent.keyDown(screen.getByRole('textbox', { name: 'Message' }), { key: 'Enter' });
    fireEvent.click(screen.getByRole('link', { name: 'Chats' }));
  }
  chooseForum('lobby');
  fireEvent.click(screen.getByRole('button', { name: 'New Session' }));
  expectSendBlocked(api);
  fireEvent.keyDown(screen.getByRole('textbox'), { key: 'Enter' });
  expect(api.createSession).toHaveBeenCalledTimes(2);
});

it('keeps input and Stop pending independently across sessions', async () => {
  vi.useFakeTimers();
  const input = deferred<void>();
  const stop = deferred<void>();
  const api = client({
    submitInput: vi.fn(() => input.promise),
    stopSession: vi.fn(() => stop.promise),
  });
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  for (const name of [/^Planning/, /^Older/]) {
    fireEvent.click(screen.getByRole('button', { name }));
    await act(() => vi.advanceTimersByTimeAsync(0));
    fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Send once' } });
    fireEvent.keyDown(screen.getByRole('textbox', { name: 'Message' }), { key: 'Enter' });
    fireEvent.click(screen.getByRole('button', { name: 'Stop' }));
    fireEvent.click(screen.getByRole('link', { name: 'Chats' }));
    await act(() => vi.advanceTimersByTimeAsync(0));
  }
  fireEvent.click(screen.getByRole('button', { name: /^Planning/ }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.getByRole('status')).toHaveTextContent('Sending');
  expect(screen.getByRole('button', { name: 'Stop' })).toBeDisabled();
  expect(api.submitInput).toHaveBeenCalledTimes(2);
  expect(api.stopSession).toHaveBeenCalledTimes(2);
});

it('waits for a fresh snapshot before Send after an uncertain input', async () => {
  vi.useFakeTimers();
  const oldRead = deferred<SessionSnapshot>();
  const freshRead = deferred<SessionSnapshot>();
  const input = deferred<void>();
  const api = client({
    getSession: vi.fn().mockResolvedValueOnce(snapshot('planning'))
      .mockImplementationOnce(() => oldRead.promise)
      .mockImplementationOnce(() => freshRead.promise),
    submitInput: vi.fn(() => input.promise),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Maybe accepted' } });
  fireEvent.keyDown(screen.getByRole('textbox', { name: 'Message' }), { key: 'Enter' });
  fireEvent(document, new Event('visibilitychange'));
  await act(async () => input.reject(new ChaWebError(500, 'Unknown', 'command_timeout')));
  await act(async () => oldRead.resolve(snapshot('planning')));
  expectSendBlocked(api);
  fireEvent.keyDown(screen.getByRole('textbox'), { key: 'Enter' });
  expect(api.submitInput).toHaveBeenCalledTimes(1);
  await act(async () => freshRead.resolve(snapshot('planning')));
  expect(screen.getByRole('alert')).toHaveTextContent(unknownSendNotice);
  expect(api.submitInput).toHaveBeenCalledTimes(1);
  expect(screen.getByRole('textbox')).toHaveValue('Maybe accepted');
  vi.mocked(api.submitInput).mockImplementation(() => new Promise(() => {}));
  fireEvent.keyDown(screen.getByRole('textbox'), { key: 'Enter' });
  expect(api.submitInput).toHaveBeenCalledTimes(2);
});

it('does not count a failed list as inspection after an uncertain creation', async () => {
  vi.useFakeTimers();
  const api = client({
    createSession: vi.fn().mockRejectedValue(new ChaWebError(0, 'Unknown')),
    listSessions: vi.fn().mockResolvedValueOnce(lobbySessions)
      .mockRejectedValue(new ChaProtocolError()),
  });
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.click(screen.getByRole('button', { name: 'New Session' }));
  fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Maybe created' } });
  fireEvent.keyDown(screen.getByRole('textbox', { name: 'Message' }), { key: 'Enter' });
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.click(screen.getByRole('link', { name: 'Chats' }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.click(screen.getByRole('button', { name: 'New Session' }));
  expectSendBlocked(api);
});

it('bounds recovery when snapshots succeed but bootstrap keeps failing', async () => {
  vi.useFakeTimers();
  const api = client({
    getBootstrap: vi.fn().mockResolvedValueOnce(boot()).mockRejectedValue(new TypeError()),
    getSession: vi.fn().mockRejectedValueOnce(new TypeError())
      .mockResolvedValue(snapshot('planning')),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  await act(() => vi.advanceTimersByTimeAsync(17_000));
  expect(screen.getByRole('button', { name: 'Retry' })).toBeInTheDocument();
  expect(api.getBootstrap).toHaveBeenCalledTimes(5);
  await act(() => vi.advanceTimersByTimeAsync(60_000));
  expect(api.getBootstrap).toHaveBeenCalledTimes(5);
  expect(api.getSession).toHaveBeenCalledTimes(5);
});

it('keeps accepted input pending until a read started after its acknowledgement finishes', async () => {
  vi.useFakeTimers();
  const oldRead = deferred<SessionSnapshot>();
  const freshRead = deferred<SessionSnapshot>();
  const input = deferred<void>();
  const api = client({
    getSession: vi.fn().mockResolvedValueOnce(snapshot('planning'))
      .mockImplementationOnce(() => oldRead.promise)
      .mockImplementationOnce(() => freshRead.promise),
    submitInput: vi.fn(() => input.promise),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Submitted' } });
  fireEvent.keyDown(screen.getByRole('textbox', { name: 'Message' }), { key: 'Enter' });
  fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Next turn' } });
  fireEvent(document, new Event('visibilitychange'));
  await act(async () => input.resolve());
  await act(async () => oldRead.resolve(snapshot('planning')));
  expect(screen.getByRole('status')).toHaveTextContent('Submitted');
  expectSendBlocked(api);
  expect(api.getSession).toHaveBeenCalledTimes(3);
  await act(async () => freshRead.resolve(snapshot('planning')));
  expect(screen.queryByRole('status')).not.toBeInTheDocument();
  expect(screen.getByRole('textbox')).toHaveValue('Next turn');
  vi.mocked(api.submitInput).mockImplementation(() => new Promise(() => {}));
  fireEvent.keyDown(screen.getByRole('textbox'), { key: 'Enter' });
  expect(api.submitInput).toHaveBeenCalledTimes(2);
});

it('waits for fresh state when reopening the current idle session', async () => {
  vi.useFakeTimers();
  const read = deferred<SessionSnapshot>();
  const api = client({ getSession: vi.fn().mockResolvedValueOnce(snapshot('planning'))
    .mockImplementationOnce(() => read.promise) });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Draft' } });
  fireEvent.click(screen.getByRole('link', { name: 'Chats' }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.click(screen.getByRole('button', { name: /^Planning/ }));
  expect(screen.getByText('First note')).toBeInTheDocument();
  expectSendBlocked(api);
  await act(async () => read.resolve(snapshot('planning')));
  vi.mocked(api.submitInput).mockImplementation(() => new Promise(() => {}));
  fireEvent.keyDown(screen.getByRole('textbox'), { key: 'Enter' });
  expect(api.submitInput).toHaveBeenCalledWith('lobby', 'planning', 'Draft');
});

it('pauses startup retries while hidden and resumes on return', async () => {
  vi.useFakeTimers();
  let visible = true;
  Object.defineProperty(document, 'visibilityState', {
    configurable: true, get: () => visible ? 'visible' : 'hidden',
  });
  const api = client({ getBootstrap: vi.fn().mockRejectedValueOnce(new TypeError()).mockResolvedValue(boot()) });
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  visible = false;
  fireEvent(document, new Event('visibilitychange'));
  await act(() => vi.advanceTimersByTimeAsync(20_000));
  expect(api.getBootstrap).toHaveBeenCalledTimes(1);
  visible = true;
  fireEvent(document, new Event('visibilitychange'));
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(api.getBootstrap).toHaveBeenCalledTimes(2);
  expect(screen.getByRole('button', { name: 'Forum' })).toBeInTheDocument();
});

it('preserves edits in a new session opened before creation returns', async () => {
  vi.useFakeTimers();
  const creation = deferred<{ id: string; label: string }>();
  const api = client({
    createSession: vi.fn(() => creation.promise),
    listSessions: vi.fn().mockResolvedValueOnce(lobbySessions).mockResolvedValue([
      { id: 'fresh', label: 'Fresh', live: true, updated_at: 100 },
    ]),
  });
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.click(screen.getByRole('button', { name: 'New Session' }));
  fireEvent.change(screen.getByRole('textbox'), { target: { value: 'First input' } });
  fireEvent.keyDown(screen.getByRole('textbox', { name: 'Message' }), { key: 'Enter' });
  fireEvent.click(screen.getByRole('link', { name: 'Chats' }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.click(screen.getByRole('button', { name: /^Fresh/ }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Next input' } });
  await act(async () => creation.resolve({ id: 'fresh', label: 'Fresh' }));
  expect(screen.getByRole('textbox')).toHaveValue('Next input');
  expect(window.location.hash).toContain('/sessions/fresh');
});

it('focuses the creation prompt instead of the end of its first response', async () => {
  const user = userEvent.setup();
  const loaded = snapshot('fresh');
  loaded.transcript = [
    { ...loaded.transcript[0], text: 'New prompt' },
    { ...loaded.transcript[0], id: 2, kind: 'character', text: 'Long response '.repeat(100) },
  ];
  vi.spyOn(HTMLElement.prototype, 'getBoundingClientRect').mockImplementation(function (this: HTMLElement) {
    const parent = this.closest('.chaweb-transcript') as HTMLElement | null;
    return { top: this.dataset.entryId ? Number(this.dataset.entryId) * 100 - (parent?.scrollTop ?? 0) : 0 } as DOMRect;
  });
  await showList(client({
    createSession: vi.fn(async () => ({ id: 'fresh', label: 'Fresh' })),
    getSession: vi.fn(async () => loaded),
  }));
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  const container = screen.getByLabelText('Conversation transcript');
  Object.defineProperties(container, { scrollHeight: { value: 2000 }, clientHeight: { value: 200 } });
  await user.type(screen.getByRole('textbox', { name: 'Message' }), 'New prompt');
  await user.click(screen.getByRole('textbox', { name: 'Message' }));
  await user.keyboard('{Enter}');
  await screen.findByText('New prompt');
  expect(container.scrollTop).toBe(100);
});
