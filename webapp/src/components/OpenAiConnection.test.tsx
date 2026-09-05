import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { ChaError, type OpenAiAuth } from '../api/client';
import { initialAppState } from '../state/view';
import {
  connectedAuth,
  fixtureClient,
  signedOutAuth,
  waitingAuth,
} from '../test/fixtures';
import { OpenAiConnectionScreen } from './OpenAiConnection';

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (reason?: unknown) => void;
  const promise = new Promise<T>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

function renderConnection(client = fixtureClient()) {
  return render(
    <OpenAiConnectionScreen
      client={client}
      sessionReport={null}
      state={initialAppState}
    />,
  );
}

async function flush() {
  await act(async () => {
    await Promise.resolve();
  });
}

describe('OpenAI connection screen', () => {
  it('does not offer Connect ChatGPT before status is known', () => {
    const getOpenAiAuth = vi.fn(() => deferred<OpenAiAuth>().promise);
    renderConnection(fixtureClient({ getOpenAiAuth }));

    expect(screen.getByRole('status')).toHaveTextContent('Loading ChatGPT connection');
    expect(screen.queryByRole('button', { name: 'Connect ChatGPT' })).not.toBeInTheDocument();
  });

  it('renders signed-out, waiting, and connected from the snapshot', async () => {
    const { unmount } = renderConnection();
    expect(await screen.findByRole('button', { name: 'Connect ChatGPT' })).toBeEnabled();
    unmount();

    renderConnection(fixtureClient({ getOpenAiAuth: async () => waitingAuth }));
    expect(await screen.findByText('TEST-ONLY')).toBeInTheDocument();
    const link = screen.getByRole('link', { name: 'the ChatGPT device page' });
    expect(link).toHaveAttribute('href', 'https://auth.openai.com/codex/device');
    expect(link).toHaveAttribute('target', '_blank');
    expect(link).toHaveAttribute('rel', 'noopener noreferrer');
    expect(screen.getByRole('button', { name: 'Cancel' })).toBeEnabled();
    expect(screen.queryByRole('button', { name: 'Connect ChatGPT' })).not.toBeInTheDocument();

    unmount();
    renderConnection(fixtureClient({ getOpenAiAuth: async () => connectedAuth }));
    expect(await screen.findByText('Connected to ChatGPT')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Disconnect' })).toBeEnabled();
  });

  it('starts login from Connect ChatGPT and shows the waiting code', async () => {
    const startOpenAiAuth = vi.fn(async () => waitingAuth);
    renderConnection(fixtureClient({ startOpenAiAuth }));
    fireEvent.click(await screen.findByRole('button', { name: 'Connect ChatGPT' }));

    expect(await screen.findByText('TEST-ONLY')).toBeInTheDocument();
    expect(startOpenAiAuth).toHaveBeenCalledTimes(1);
  });

  it('disconnects a connected account', async () => {
    const disconnectOpenAiAuth = vi.fn(async () => signedOutAuth);
    renderConnection(fixtureClient({
      getOpenAiAuth: async () => connectedAuth,
      disconnectOpenAiAuth,
    }));
    fireEvent.click(await screen.findByRole('button', { name: 'Disconnect' }));

    expect(await screen.findByRole('button', { name: 'Connect ChatGPT' })).toBeEnabled();
    expect(disconnectOpenAiAuth).toHaveBeenCalledTimes(1);
  });

  it('resumes a pending attempt from GET without starting another login', async () => {
    const startOpenAiAuth = vi.fn(async () => waitingAuth);
    renderConnection(fixtureClient({
      getOpenAiAuth: async () => waitingAuth,
      startOpenAiAuth,
    }));

    expect(await screen.findByText('TEST-ONLY')).toBeInTheDocument();
    expect(startOpenAiAuth).not.toHaveBeenCalled();
  });

  it('opens the verification URL only after a click, with new-tab protections', async () => {
    const open = vi.spyOn(window, 'open');
    renderConnection(fixtureClient({ getOpenAiAuth: async () => waitingAuth }));
    const link = await screen.findByRole('link', {
      name: 'the ChatGPT device page',
    });

    expect(open).not.toHaveBeenCalled();
    expect(link).toHaveAttribute('target', '_blank');
    expect(link).toHaveAttribute('rel', 'noopener noreferrer');
    fireEvent.click(link);
    expect(open).not.toHaveBeenCalled();
    open.mockRestore();
  });

  it('does not render a non-https verification URL as a link', async () => {
    renderConnection(fixtureClient({
      getOpenAiAuth: async () => ({ ...waitingAuth, verification_url: 'javascript:alert(1)' }),
    }));
    expect(await screen.findByText('TEST-ONLY')).toBeInTheDocument();
    expect(screen.queryByRole('link')).not.toBeInTheDocument();
  });

  it('keeps only displayable status, code, and timing in the page', async () => {
    renderConnection(fixtureClient({ getOpenAiAuth: async () => waitingAuth }));
    expect(await screen.findByText('TEST-ONLY')).toBeInTheDocument();
    expect(document.body.textContent).not.toMatch(/access_token|refresh_token|account_id/);
  });

  it('shows an API error with an explicit retry that reloads status', async () => {
    const getOpenAiAuth = vi.fn()
      .mockRejectedValueOnce(new ChaError(500, 'internal_error', 'OpenAI login failed.'))
      .mockResolvedValueOnce(signedOutAuth);
    renderConnection(fixtureClient({ getOpenAiAuth }));

    expect(await screen.findByRole('alert')).toHaveTextContent('OpenAI login failed.');
    expect(screen.queryByRole('button', { name: 'Connect ChatGPT' })).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Try again' }));
    expect(await screen.findByRole('button', { name: 'Connect ChatGPT' })).toBeEnabled();
    expect(getOpenAiAuth).toHaveBeenCalledTimes(2);
  });

  it('refuses a second Connect while login is in flight', async () => {
    const login = deferred<OpenAiAuth>();
    const startOpenAiAuth = vi.fn(() => login.promise);
    renderConnection(fixtureClient({ startOpenAiAuth }));
    fireEvent.click(await screen.findByRole('button', { name: 'Connect ChatGPT' }));

    expect(screen.getByRole('button', { name: 'Connect ChatGPT' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Connect ChatGPT' }));
    expect(startOpenAiAuth).toHaveBeenCalledTimes(1);
    await act(async () => { login.resolve(waitingAuth); });
    expect(await screen.findByText('TEST-ONLY')).toBeInTheDocument();
  });
});

describe('OpenAI connection polling', () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('schedules one poll from the returned delay and waits for it before the next', async () => {
    const first = deferred<OpenAiAuth>();
    const pollOpenAiAuth = vi.fn()
      .mockImplementationOnce(() => first.promise)
      .mockResolvedValueOnce({ ...waitingAuth, next_poll_delay_ms: 2000 })
      .mockResolvedValue(connectedAuth);
    renderConnection(fixtureClient({
      getOpenAiAuth: async () => waitingAuth,
      pollOpenAiAuth,
    }));
    await flush();

    expect(pollOpenAiAuth).not.toHaveBeenCalled();
    await act(async () => { vi.advanceTimersByTime(999); });
    expect(pollOpenAiAuth).not.toHaveBeenCalled();
    await act(async () => { vi.advanceTimersByTime(1); });
    expect(pollOpenAiAuth).toHaveBeenCalledTimes(1);

    await act(async () => { vi.advanceTimersByTime(5_000); });
    expect(pollOpenAiAuth).toHaveBeenCalledTimes(1);

    await act(async () => { first.resolve({ ...waitingAuth, next_poll_delay_ms: 2000 }); });
    await act(async () => { vi.advanceTimersByTime(1999); });
    expect(pollOpenAiAuth).toHaveBeenCalledTimes(1);
    await act(async () => { vi.advanceTimersByTime(1); });
    expect(pollOpenAiAuth).toHaveBeenCalledTimes(2);
  });

  it('falls back to a safe delay when the poll delay is missing', async () => {
    const pollOpenAiAuth = vi.fn(async () => connectedAuth);
    renderConnection(fixtureClient({
      getOpenAiAuth: async () => ({ ...waitingAuth, next_poll_delay_ms: undefined }),
      pollOpenAiAuth,
    }));
    await flush();

    await act(async () => { vi.advanceTimersByTime(999); });
    expect(pollOpenAiAuth).not.toHaveBeenCalled();
    await act(async () => { vi.advanceTimersByTime(1); });
    expect(pollOpenAiAuth).toHaveBeenCalledTimes(1);
  });

  it('does not reschedule after a connected poll, an error, cancel, or unmount', async () => {
    const pollOpenAiAuth = vi.fn(async () => connectedAuth);
    const { unmount } = renderConnection(fixtureClient({
      getOpenAiAuth: async () => waitingAuth,
      pollOpenAiAuth,
    }));
    await flush();
    await act(async () => { vi.advanceTimersByTime(1000); });
    await flush();
    expect(screen.getByText('Connected to ChatGPT')).toBeInTheDocument();
    await act(async () => { vi.advanceTimersByTime(10_000); });
    expect(pollOpenAiAuth).toHaveBeenCalledTimes(1);
    unmount();

    const failingPoll = vi.fn(async () => {
      throw new ChaError(500, 'internal_error', 'OpenAI login failed.');
    });
    renderConnection(fixtureClient({
      getOpenAiAuth: async () => waitingAuth,
      pollOpenAiAuth: failingPoll,
    }));
    await flush();
    await act(async () => { vi.advanceTimersByTime(1000); });
    await flush();
    expect(screen.getByRole('alert')).toHaveTextContent('OpenAI login failed.');
    await act(async () => { vi.advanceTimersByTime(10_000); });
    expect(failingPoll).toHaveBeenCalledTimes(1);
  });

  it('cancels a pending attempt without scheduling another poll', async () => {
    const poll = deferred<OpenAiAuth>();
    const pollOpenAiAuth = vi.fn(() => poll.promise);
    const disconnectOpenAiAuth = vi.fn(async () => signedOutAuth);
    renderConnection(fixtureClient({
      getOpenAiAuth: async () => waitingAuth,
      pollOpenAiAuth,
      disconnectOpenAiAuth,
    }));
    await flush();
    await act(async () => { vi.advanceTimersByTime(1000); });
    expect(pollOpenAiAuth).toHaveBeenCalledTimes(1);

    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    await act(async () => { poll.resolve(waitingAuth); });
    await act(async () => { vi.advanceTimersByTime(10_000); });
    expect(pollOpenAiAuth).toHaveBeenCalledTimes(1);
    expect(disconnectOpenAiAuth).toHaveBeenCalledTimes(1);
    expect(screen.getByRole('button', { name: 'Connect ChatGPT' })).toBeEnabled();
  });

  it('ignores a late poll after unmount and does not recreate a timer', async () => {
    const poll = deferred<OpenAiAuth>();
    const pollOpenAiAuth = vi.fn(() => poll.promise);
    const { unmount } = renderConnection(fixtureClient({
      getOpenAiAuth: async () => waitingAuth,
      pollOpenAiAuth,
    }));
    await flush();
    await act(async () => { vi.advanceTimersByTime(1000); });
    expect(pollOpenAiAuth).toHaveBeenCalledTimes(1);

    unmount();
    await act(async () => { poll.resolve(waitingAuth); });
    expect(vi.getTimerCount()).toBe(0);
    await act(async () => { vi.advanceTimersByTime(10_000); });
    expect(pollOpenAiAuth).toHaveBeenCalledTimes(1);
  });
});
