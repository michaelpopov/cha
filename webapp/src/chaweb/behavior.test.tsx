import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, expect, it, vi } from 'vitest';

import type { SessionListing, SessionSnapshot } from '../api/client';
import { ChaProtocolError } from '../api/client';
import { bootstrapFixture, plainVoice, snapshotFixture } from '../test/fixtures';
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
    reasoning_text: 'hidden reasoning',
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
  vi.useRealTimers();
  Reflect.deleteProperty(document, 'visibilityState');
  Reflect.deleteProperty(window, 'visualViewport');
});

function client(overrides: Partial<ChaWebClient> = {}): ChaWebClient {
  return {
    getVoiceOutputRuntime: vi.fn(async () => null),
    startAudio: vi.fn(),
    getAudioStatus: vi.fn(),
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

async function openPlanning(api = client()) {
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await screen.findByRole('textbox', { name: 'Message' });
  await screen.findByText('First note');
  return api;
}

it('confirms the named session, focuses Cancel, and preserves the conversation on cancellation', async () => {
  const user = userEvent.setup();
  const api = await openPlanning();
  const box = screen.getByRole('textbox', { name: 'Message' });
  await user.type(box, 'Unsent note');
  const controls = screen.getByRole('button', { name: 'Delete session' }).parentElement!;
  expect(within(controls).getAllByRole('button').map((button) => button.getAttribute('aria-label')))
    .toEqual(['Sessions', 'Delete session', 'Copy conversation', 'Send']);
  await user.click(screen.getByRole('button', { name: 'Delete session' }));
  const dialog = screen.getByRole('dialog', { name: 'Delete session “planning”?' });
  expect(dialog).toHaveTextContent('The unsent prompt will also be discarded.');
  expect(within(dialog).getByRole('button', { name: 'Cancel' })).toHaveFocus();
  await user.keyboard('{Enter}');
  expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Delete session' })).toHaveFocus();
  expect(api.deleteSession).not.toHaveBeenCalled();
  expect(box).toHaveValue('Unsent note');
  expect(window.location.hash).toContain('/planning');
});

it('returns to the same forum after deletion, clears its URL and draft, and keeps other drafts', async () => {
  const user = userEvent.setup();
  const deleted = deferred<void>();
  let removed = false;
  const api = await showList(client({
    deleteSession: vi.fn(() => deleted.promise.then(() => { removed = true; })),
    listSessions: vi.fn(async () => removed ? [lobbySessions[0]!] : lobbySessions),
  }));
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  await user.type(screen.getByRole('textbox'), 'Separate new draft');
  await user.click(screen.getByRole('button', { name: 'Sessions' }));
  await user.click(await screen.findByRole('button', { name: /Planning/ }));
  await screen.findByText('First note');
  await user.type(screen.getByRole('textbox'), 'Deleted draft');
  await user.click(screen.getByRole('button', { name: 'Delete session' }));
  await user.click(screen.getByRole('button', { name: 'Delete' }));
  expect(api.deleteSession).toHaveBeenCalledExactlyOnceWith('lobby', 'planning');
  expect(screen.getByRole('status')).toHaveTextContent('Deleting');
  expect(screen.getByRole('button', { name: 'Sessions' })).toBeDisabled();
  expect(screen.getByRole('button', { name: 'Delete session' })).toBeDisabled();
  expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
  await act(async () => deleted.resolve());
  expect(await screen.findByRole('button', { name: 'Forum' })).toHaveTextContent('The Lobby');
  await screen.findByRole('button', { name: /Older/ });
  expect(screen.queryByRole('button', { name: /Planning/ })).not.toBeInTheDocument();
  expect(window.location.hash).toBe('');
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  expect(screen.getByRole('textbox')).toHaveValue('Separate new draft');
  // Even an old history entry cannot restore the deleted session's draft.
  await settleHistory(() => {
    window.history.pushState(null, '', '/#/forums/lobby/sessions/planning');
    window.dispatchEvent(new PopStateEvent('popstate'));
  });
  await screen.findByText('First note');
  expect(screen.getByRole('textbox')).toHaveValue('');
});

it('keeps the session and draft when deletion fails and permits an explicit retry', async () => {
  const user = userEvent.setup();
  const api = await openPlanning(client({
    deleteSession: vi.fn(async () => { throw new ChaWebError(500, 'Try again.', 'session_stopping'); }),
  }));
  await user.type(screen.getByRole('textbox'), 'Keep this');
  await user.click(screen.getByRole('button', { name: 'Delete session' }));
  await user.click(screen.getByRole('button', { name: 'Delete' }));
  expect(await screen.findByRole('alert')).toHaveTextContent('Try again.');
  expect(screen.getByRole('textbox')).toHaveValue('Keep this');
  expect(screen.queryByRole('button', { name: 'Forum' })).not.toBeInTheDocument();
  expect(window.location.hash).toContain('/planning');
  expect(screen.getByRole('button', { name: 'Delete session' })).toBeEnabled();
  expect(api.deleteSession).toHaveBeenCalledTimes(1);
});

it('disables deletion for unsent sessions and while input or Stop is pending', async () => {
  const user = userEvent.setup();
  const input = deferred<void>();
  const stopped = deferred<void>();
  const api = await showList(client({
    submitInput: vi.fn(() => input.promise),
    stopSession: vi.fn(() => stopped.promise),
  }));
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  expect(screen.getByRole('button', { name: 'Delete session' })).toBeDisabled();
  await user.click(screen.getByRole('button', { name: 'Sessions' }));
  await user.click(await screen.findByRole('button', { name: /Planning/ }));
  await screen.findByText('First note');
  await user.type(screen.getByRole('textbox'), 'Hello');
  await user.click(screen.getByRole('button', { name: 'Send' }));
  expect(screen.getByRole('button', { name: 'Delete session' })).toBeDisabled();
  await user.click(screen.getByRole('button', { name: 'Stop' }));
  expect(screen.getByRole('button', { name: 'Delete session' })).toBeDisabled();
  expect(api.deleteSession).not.toHaveBeenCalled();
});

it('warns about stopping an active reply and ignores an in-flight snapshot during deletion', async () => {
  vi.useFakeTimers();
  const late = deferred<SessionSnapshot>();
  const deleted = deferred<void>();
  const api = client({
    getSession: vi.fn()
      .mockResolvedValueOnce(snapshot('planning', { generation: generation(true) }))
      .mockImplementationOnce(() => late.promise),
    deleteSession: vi.fn(() => deleted.promise),
  });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  await act(() => vi.advanceTimersByTimeAsync(1000));
  expect(api.getSession).toHaveBeenCalledTimes(2);
  fireEvent.click(screen.getByRole('button', { name: 'Delete session' }));
  expect(screen.getByRole('dialog')).toHaveTextContent('The current reply will be stopped.');
  fireEvent.click(screen.getByRole('button', { name: 'Delete' }));
  expect(screen.getByRole('button', { name: 'Stop' })).toBeDisabled();
  await act(async () => {
    late.resolve(snapshot('planning', { session_label: 'Late title' }));
    await vi.advanceTimersByTimeAsync(0);
  });
  expect(screen.getByRole('status')).toHaveTextContent('Deleting');
  expect(api.getSession).toHaveBeenCalledTimes(2);
  await act(async () => {
    deleted.resolve();
    await vi.advanceTimersByTimeAsync(0);
  });
  expect(screen.getByRole('button', { name: 'Forum' })).toHaveTextContent('The Lobby');
  expect(screen.queryByRole('textbox')).not.toBeInTheDocument();
});

it('does not leave another conversation when deletion finishes after browser navigation', async () => {
  const user = userEvent.setup();
  const deleted = deferred<void>();
  await openPlanning(client({ deleteSession: vi.fn(() => deleted.promise) }));
  await user.click(screen.getByRole('button', { name: 'Delete session' }));
  await user.click(screen.getByRole('button', { name: 'Delete' }));
  await settleHistory(() => {
    window.history.pushState(null, '', '/#/forums/lobby/sessions/older');
    window.dispatchEvent(new PopStateEvent('popstate'));
  });
  await user.type(screen.getByRole('textbox'), 'Other session draft');
  await act(async () => deleted.resolve());
  expect(screen.getByRole('textbox')).toHaveValue('Other session draft');
  expect(window.location.hash).toContain('/older');
  expect(screen.queryByRole('button', { name: 'Forum' })).not.toBeInTheDocument();
});

it('treats a session already deleted elsewhere as deleted and closes stale confirmations on navigation', async () => {
  const user = userEvent.setup();
  const api = await openPlanning(client({
    deleteSession: vi.fn(async () => { throw new ChaWebError(404, 'Not found.', 'not_found'); }),
  }));
  await user.click(screen.getByRole('button', { name: 'Delete session' }));
  await settleHistory(() => {
    window.history.pushState(null, '', '/#/forums/lobby/sessions/older');
    window.dispatchEvent(new PopStateEvent('popstate'));
  });
  expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  expect(api.deleteSession).not.toHaveBeenCalled();
  await user.click(screen.getByRole('button', { name: 'Delete session' }));
  await user.click(screen.getByRole('button', { name: 'Delete' }));
  await screen.findByRole('button', { name: 'Forum' });
  expect(api.deleteSession).toHaveBeenCalledExactlyOnceWith('lobby', 'older');
  expect(window.location.hash).toBe('');
});

it('keeps one new draft per forum and does not create a session', async () => {
  const user = userEvent.setup();
  const api = await showList();
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  await user.type(screen.getByRole('textbox', { name: 'Message' }), 'Lobby draft');
  await user.click(screen.getByRole('button', { name: 'Sessions' }));
  await user.click(screen.getByRole('button', { name: 'Forum' }));
  await user.click(screen.getByRole('button', { name: /^Archive / }));
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  const archive = screen.getByRole('textbox', { name: 'Message' });
  expect(archive).toHaveValue('');
  await user.type(archive, 'Archive draft');
  await user.click(screen.getByRole('button', { name: 'Sessions' }));
  await user.click(screen.getByRole('button', { name: 'Forum' }));
  await user.click(screen.getByRole('button', { name: /^The Lobby / }));
  await user.click(screen.getByRole('button', { name: 'New Session' }));
  expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('Lobby draft');
  expect(api.createSession).not.toHaveBeenCalled();
  expect(api.getSession).not.toHaveBeenCalled();
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
  await user.click(screen.getByRole('button', { name: 'Send' }));
  fireEvent.change(box, { target: { value: 'Hello!' } });
  await act(async () => {
    input.resolve();
  });
  expect(box).toHaveValue('Hello!');
  expect(api.submitInput).toHaveBeenCalledTimes(1);
  expect(api.submitInput).toHaveBeenCalledWith('lobby', 'planning', 'Hello');
  expect(screen.getByRole('status')).toHaveTextContent('Hello');
  expect(screen.queryByRole('button', { name: 'Send' })).not.toBeInTheDocument();
  await act(async () => afterAck.resolve(snapshot('planning')));

  api.submitInput = vi.fn(async () => {
    throw new ChaWebError(422, 'Unknown command', 'invalid_argument');
  });
  await user.clear(box);
  await user.type(box, '@- note');
  await user.click(screen.getByRole('button', { name: 'Send' }));
  expect(screen.getByRole('alert')).toHaveTextContent('Unknown command');
  expect(box).toHaveValue('@- note');
  expect(screen.getByRole('button', { name: 'Send' })).toBeEnabled();
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
  await user.click(await screen.findByRole('button', { name: /Planning/ }));
  await screen.findByText('First note');
  expect(api.getSession).toHaveBeenCalledTimes(1);
  await user.click(screen.getByRole('button', { name: 'Sessions' }));
  await user.click(await screen.findByRole('button', { name: /Older/ }));
  await screen.findByText('First note');
  expect(api.getSession).toHaveBeenCalledTimes(2);
  expect(api.getSession).toHaveBeenLastCalledWith('lobby', 'older');

  await settleHistory(() => window.history.back());
  expect(api.getSession).toHaveBeenCalledTimes(3);
  expect(api.getSession).toHaveBeenLastCalledWith('lobby', 'planning');
  await settleHistory(() => window.history.forward());
  expect(api.getSession).toHaveBeenCalledTimes(4);
  expect(api.getSession).toHaveBeenLastCalledWith('lobby', 'older');
});

it('ignores a stale snapshot and a snapshot for a different session', async () => {
  const user = userEvent.setup();
  const first = deferred<SessionSnapshot>();
  const second = deferred<SessionSnapshot>();
  const api = await showList(client({
    getSession: vi.fn()
      .mockImplementationOnce(() => first.promise)
      .mockImplementationOnce(() => second.promise),
  }));
  await user.click(await screen.findByRole('button', { name: /Planning/ }));
  await screen.findByRole('textbox', { name: 'Message' });
  await user.click(screen.getByRole('button', { name: 'Sessions' }));
  await user.click(await screen.findByRole('button', { name: /Older/ }));
  expect(api.getSession).toHaveBeenCalledTimes(1);
  await act(async () => {
    first.resolve(snapshot('planning', {
      transcript: [{
        id: 7,
        kind: 'human',
        participant_id: 'reader',
        display_name: 'Reader',
        addressed_to: '',
        addressed_to_name: '',
        text: 'Planning only',
        status: 'complete',
        created_at: 1,
      }],
    }));
  });
  expect(screen.queryByText('Planning only')).not.toBeInTheDocument();
  expect(api.getSession).toHaveBeenCalledTimes(2);
  await act(async () => {
    second.resolve(snapshot('planning', {
      transcript: [{
        id: 8,
        kind: 'human',
        participant_id: 'reader',
        display_name: 'Reader',
        addressed_to: '',
        addressed_to_name: '',
        text: 'Wrong session',
        status: 'complete',
        created_at: 1,
      }],
    }));
  });
  expect(screen.getByRole('alert')).toHaveTextContent('incompatible response');
  expect(screen.queryByText('Wrong session')).not.toBeInTheDocument();
  expect(api.getSession).toHaveBeenCalledTimes(2);
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
  await user.click(screen.getByRole('button', { name: 'Send' }));
  fireEvent.change(box, { target: { value: '@Guide hello!' } });
  await user.click(screen.getByRole('button', { name: 'Sessions' }));
  await user.click(screen.getByRole('button', { name: 'Forum' }));
  await user.click(screen.getByRole('button', { name: /^Archive / }));
  await user.click(screen.getByRole('button', { name: /Stored/ }));
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
  await user.click(screen.getByRole('button', { name: 'Sessions' }));
  await user.click(screen.getByRole('button', { name: 'Forum' }));
  await user.click(screen.getByRole('button', { name: /^The Lobby / }));
  await user.click(await screen.findByRole('button', { name: /Fresh/ }));
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
  await user.click(screen.getByRole('button', { name: 'Send' }));
  await user.click(screen.getByRole('button', { name: 'Sessions' }));
  await user.click(screen.getByRole('button', { name: /Planning/ }));
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
  await user.click(screen.getByRole('button', { name: 'Send' }));
  expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
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
  await user.click(screen.getByRole('button', { name: 'Send' }));
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
  fireEvent.click(screen.getByRole('button', { name: 'Send' }));
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
  expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
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
  fireEvent.click(screen.getByRole('button', { name: 'Sessions' }));
  fireEvent.click(screen.getByRole('button', { name: /Planning/ }));
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
  fireEvent.click(screen.getByRole('button', { name: 'Sessions' }));
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
  fireEvent.click(screen.getByRole('button', { name: 'Send' }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.getByRole('alert')).toHaveTextContent(unknownSendNotice);
  expect(box).toHaveValue('Maybe sent');
  expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
  await act(() => vi.advanceTimersByTimeAsync(30_000));
  expect(api.createSession).toHaveBeenCalledTimes(1);
  expect(screen.queryByRole('button', { name: /Maybe sent/ })).not.toBeInTheDocument();
  fireEvent.click(screen.getByRole('button', { name: 'Sessions' }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.queryByRole('button', { name: /fresh/i })).not.toBeInTheDocument();
  fireEvent.click(screen.getByRole('button', { name: 'New Session' }));
  expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
  fireEvent.click(screen.getByRole('button', { name: 'Allow another Send' }));
  expect(screen.getByRole('button', { name: 'Send' })).toBeEnabled();
  expect(api.createSession).toHaveBeenCalledTimes(1);
  fireEvent.click(screen.getByRole('button', { name: 'Send' }));
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
  fireEvent.click(screen.getByRole('button', { name: 'Send' }));
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
  await user.click(screen.getByRole('button', { name: 'Send' }));
  expect(screen.getByRole('alert')).toHaveTextContent('Prompt is too large.');
  expect(box).toHaveValue('Too big');
  expect(window.location.hash).toBe('');
  expect(screen.getByRole('button', { name: 'Send' })).toBeEnabled();
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

it('does not send dictated text by itself', async () => {
  const api = await openPlanning();
  const box = screen.getByRole('textbox', { name: 'Message' });
  fireEvent.focus(box);
  fireEvent.compositionStart(box);
  fireEvent.change(box, { target: { value: 'Spoken words' } });
  fireEvent.compositionEnd(box);
  fireEvent.input(box, { target: { value: 'Spoken words' } });
  expect(box).toHaveValue('Spoken words');
  expect(api.submitInput).not.toHaveBeenCalled();
  expect(api.createSession).not.toHaveBeenCalled();
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
    fireEvent.click(screen.getByRole('button', { name: 'Send' }));
    fireEvent.click(screen.getByRole('button', { name: 'Sessions' }));
  }
  chooseForum('lobby');
  fireEvent.click(screen.getByRole('button', { name: 'New Session' }));
  expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
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
  for (const name of [/Planning/, /Older/]) {
    fireEvent.click(screen.getByRole('button', { name }));
    await act(() => vi.advanceTimersByTimeAsync(0));
    fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Send once' } });
    fireEvent.click(screen.getByRole('button', { name: 'Send' }));
    fireEvent.click(screen.getByRole('button', { name: 'Stop' }));
    fireEvent.click(screen.getByRole('button', { name: 'Sessions' }));
    await act(() => vi.advanceTimersByTimeAsync(0));
  }
  fireEvent.click(screen.getByRole('button', { name: /Planning/ }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  expect(screen.getByRole('status')).toHaveTextContent('Sending');
  expect(screen.getByRole('button', { name: 'Stop' })).toBeDisabled();
  expect(api.submitInput).toHaveBeenCalledTimes(2);
  expect(api.stopSession).toHaveBeenCalledTimes(2);
});

it('requires a fresh snapshot and a deliberate decision after an uncertain input', async () => {
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
  fireEvent.click(screen.getByRole('button', { name: 'Send' }));
  fireEvent(document, new Event('visibilitychange'));
  await act(async () => input.reject(new ChaWebError(500, 'Unknown', 'command_timeout')));
  await act(async () => oldRead.resolve(snapshot('planning')));
  expect(screen.queryByRole('button', { name: 'Allow another Send' })).not.toBeInTheDocument();
  await act(async () => freshRead.resolve(snapshot('planning')));
  expect(screen.queryByRole('button', { name: 'Send' })).not.toBeInTheDocument();
  fireEvent.keyDown(screen.getByRole('textbox'), { key: 'Enter' });
  expect(api.submitInput).toHaveBeenCalledTimes(1);
  fireEvent.click(screen.getByRole('button', { name: 'Allow another Send' }));
  expect(screen.getByRole('button', { name: 'Send' })).toBeEnabled();
  expect(api.submitInput).toHaveBeenCalledTimes(1);
  expect(screen.getByRole('textbox')).toHaveValue('Maybe accepted');
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
  fireEvent.click(screen.getByRole('button', { name: 'Send' }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.click(screen.getByRole('button', { name: 'Sessions' }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.click(screen.getByRole('button', { name: 'New Session' }));
  expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
  expect(screen.queryByRole('button', { name: 'Allow another Send' })).not.toBeInTheDocument();
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

it('clears rows on a forum switch and ignores an old list when returning', async () => {
  vi.useFakeTimers();
  const oldList = deferred<SessionListing[]>();
  const newList = deferred<SessionListing[]>();
  const api = client({ listSessions: vi.fn().mockResolvedValueOnce(lobbySessions)
    .mockImplementationOnce(() => oldList.promise).mockImplementationOnce(() => newList.promise) });
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  chooseForum('archive');
  expect(screen.queryByRole('button', { name: /Planning/ })).not.toBeInTheDocument();
  chooseForum('lobby');
  chooseForum('archive');
  await act(async () => oldList.resolve([{ id: 'old', label: 'Stale row', live: false, updated_at: 1 }]));
  expect(screen.queryByRole('button', { name: /Stale row/ })).not.toBeInTheDocument();
  await act(async () => newList.resolve([{ id: 'new', label: 'Current row', live: false, updated_at: 2 }]));
  expect(screen.getByRole('button', { name: /Current row/ })).toBeInTheDocument();
});

it('does not keep retrying lists after leaving them or hiding the page', async () => {
  vi.useFakeTimers();
  const api = client({ listSessions: vi.fn().mockRejectedValue(new TypeError()) });
  render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.click(screen.getByRole('button', { name: 'New Session' }));
  await act(() => vi.advanceTimersByTimeAsync(20_000));
  expect(api.listSessions).toHaveBeenCalledTimes(1);
  fireEvent.click(screen.getByRole('button', { name: 'Sessions' }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  Object.defineProperty(document, 'visibilityState', { configurable: true, value: 'hidden' });
  fireEvent(document, new Event('visibilitychange'));
  await act(() => vi.advanceTimersByTimeAsync(20_000));
  expect(api.listSessions).toHaveBeenCalledTimes(2);
});

it('does not restart polling when an outstanding read finishes after unmount', async () => {
  vi.useFakeTimers();
  const read = deferred<SessionSnapshot>();
  const api = client({ getSession: vi.fn(() => read.promise) });
  window.history.replaceState(null, '', '/#/forums/lobby/sessions/planning');
  const app = render(<App client={api} />);
  await act(() => vi.advanceTimersByTimeAsync(0));
  app.unmount();
  await act(async () => read.resolve(snapshot('planning', { generation: generation(true) })));
  await act(() => vi.advanceTimersByTimeAsync(10_000));
  expect(api.getSession).toHaveBeenCalledTimes(1);
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
  fireEvent.click(screen.getByRole('button', { name: 'Send' }));
  fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Next turn' } });
  fireEvent(document, new Event('visibilitychange'));
  await act(async () => input.resolve());
  await act(async () => oldRead.resolve(snapshot('planning')));
  expect(screen.getByRole('status')).toHaveTextContent('Submitted');
  expect(screen.queryByRole('button', { name: 'Send' })).not.toBeInTheDocument();
  expect(api.getSession).toHaveBeenCalledTimes(3);
  await act(async () => freshRead.resolve(snapshot('planning')));
  expect(screen.queryByRole('status')).not.toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Send' })).toBeEnabled();
  expect(screen.getByRole('textbox')).toHaveValue('Next turn');
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
  fireEvent.click(screen.getByRole('button', { name: 'Sessions' }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.click(screen.getByRole('button', { name: /Planning/ }));
  expect(screen.getByText('First note')).toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'Send' })).not.toBeInTheDocument();
  await act(async () => read.resolve(snapshot('planning')));
  expect(screen.getByRole('button', { name: 'Send' })).toBeEnabled();
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
  fireEvent.click(screen.getByRole('button', { name: 'Send' }));
  fireEvent.click(screen.getByRole('button', { name: 'Sessions' }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.click(screen.getByRole('button', { name: /Fresh/ }));
  await act(() => vi.advanceTimersByTimeAsync(0));
  fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Next input' } });
  await act(async () => creation.resolve({ id: 'fresh', label: 'Fresh' }));
  expect(screen.getByRole('textbox')).toHaveValue('Next input');
  expect(window.location.hash).toContain('/sessions/fresh');
});
