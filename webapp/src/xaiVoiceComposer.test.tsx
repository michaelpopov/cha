import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { ChaClient } from './api/client';
import { ChatScreen } from './components/ChatScreen';
import { initialAppState } from './state/view';
import { bootstrapFixture, fixtureClient, snapshotFixture } from './test/fixtures';

class FakePort extends EventTarget {
  start(): void {}

  postMessage(data: { type?: string }): void {
    if (data.type !== 'flush') return;
    this.dispatchEvent(new MessageEvent('message', { data: { type: 'flushed' } }));
  }
}

class FakeWorklet {
  static latest: FakeWorklet | null = null;

  readonly port = new FakePort();

  constructor(_context: unknown, _name: string) {
    FakeWorklet.latest = this;
  }

  connect(): this { return this; }

  disconnect(): void {}

  emit(samples: Int16Array): void {
    this.port.dispatchEvent(new MessageEvent('message', {
      data: { type: 'batch', samples },
    }));
  }
}

class FakeContext {
  static latest: FakeContext | null = null;

  sampleRate = 16000;
  state: AudioContextState = 'running';
  destination = {};
  closed = false;

  constructor() {
    FakeContext.latest = this;
  }

  readonly audioWorklet = { addModule: async () => undefined };

  createGain() {
    return {
      gain: { value: 1 },
      connect: () => undefined,
      disconnect: () => undefined,
    };
  }

  createMediaStreamSource() {
    return { connect: () => undefined, disconnect: () => undefined };
  }

  async resume(): Promise<void> {}

  async close(): Promise<void> {
    this.closed = true;
    this.state = 'closed';
  }
}

const runtime = {
  provider: 'xai' as const,
  url: 'ws://127.0.0.1:9/v1/stt',
  model: 'grok-voice-transcribe-2.0',
  delay: 'low' as const,
  prompt: '',
};

function installCapture(): { stopTrack: ReturnType<typeof vi.fn> } {
  const stopTrack = vi.fn();
  Object.defineProperty(navigator, 'mediaDevices', {
    configurable: true,
    value: {
      getUserMedia: vi.fn(async () => ({
        getAudioTracks: () => [{ kind: 'audio', readyState: 'live', stop: stopTrack }],
        getTracks: () => [{ kind: 'audio', readyState: 'live', stop: stopTrack }],
      })),
    },
  });
  vi.stubGlobal('AudioContext', FakeContext);
  vi.stubGlobal('AudioWorkletNode', FakeWorklet);
  return { stopTrack };
}

function renderChat(
  client: ChaClient,
  vaultName = 'Personal',
) {
  const props = {
    client,
    dispatch: vi.fn(),
    onCoverConversation: vi.fn(async () => ({ clear_input: false })),
    onDeleteTurn: vi.fn(async () => ({ clear_input: false })),
    onRetryStream: vi.fn(),
    onReturnToWelcome: vi.fn(),
    onSetDefaultCharacter: vi.fn(async () => ({ clear_input: false })),
    onStopGeneration: vi.fn(async () => ({ clear_input: false })),
    onSubmitInput: vi.fn(async () => ({ clear_input: true })),
    onUncoverConversation: vi.fn(async () => ({ clear_input: false })),
    playbackPositions: new Map(),
  };
  const state = (name: string) => ({
    ...initialAppState,
    bootstrapStatus: 'ready' as const,
    bootstrap: { ...bootstrapFixture, vault_name: name },
    sessionSnapshot: snapshotFixture,
    streamStatus: 'connected' as const,
    currentDefaultCharacterId: snapshotFixture.default_character_id,
  });
  const view = render(<ChatScreen {...props} state={state(vaultName)} />);
  return {
    ...view,
    props,
    rerenderVault(name: string) {
      view.rerender(<ChatScreen {...props} state={state(name)} />);
    },
  };
}

async function emit(piecesForAudio: string[][], stopPieces: string[] = []) {
  const languages: string[][] = [];
  let releaseStop: ((value: { session_id: string; pieces: string[] }) => void) | undefined;
  const client = fixtureClient({
    getVoiceInputRuntime: async () => runtime,
    startXaiVoiceInput: async (sessionId, spoken) => {
      languages.push(spoken);
      return { session_id: sessionId, stop_budget_ms: 20000 };
    },
    sendXaiVoiceAudio: async (sessionId) => ({
      session_id: sessionId,
      pieces: piecesForAudio.shift() ?? [],
    }),
    stopXaiVoiceInput: (sessionId) => new Promise((resolve) => {
      releaseStop = () => resolve({ session_id: sessionId, pieces: stopPieces });
    }),
    cancelXaiVoiceInput: async () => undefined,
  });
  return { client, languages, releaseStop: () => releaseStop };
}

afterEach(() => {
  FakeWorklet.latest = null;
  FakeContext.latest = null;
  Reflect.deleteProperty(navigator, 'mediaDevices');
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

async function dictate(piecesForAudio: string[][], stopPieces: string[] = []) {
  installCapture();
  const harness = await emit(piecesForAudio, stopPieces);
  renderChat(harness.client);
  fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
  await screen.findByRole('button', { name: 'Stop voice input' });
  return harness;
}

describe('xAI composer', () => {
  it('appends finalized pieces, newlines, and a stop reply to the draft', async () => {
    const harness = await dictate([['Hello', ' new line']], [' world']);
    FakeWorklet.latest?.emit(Int16Array.from([1]));
    const input = screen.getByRole('textbox', { name: 'Message' });
    await waitFor(() => expect(input).toHaveValue('Hello\n'));
    fireEvent.click(screen.getByRole('button', { name: 'Stop voice input' }));
    expect(await screen.findByRole('button', { name: 'Finishing transcription' })).toBeDisabled();
    expect(input).toHaveValue('Hello\n');
    await act(async () => { harness.releaseStop()?.({ session_id: 'unused', pieces: [] }); });
    expect(input).toHaveValue('Hello\nworld');
    expect(screen.getByRole('button', { name: 'Start voice input' })).toBeEnabled();
  });

  it('keeps a newline-only first addition and later punctuation', async () => {
    installCapture();
    const harness = await emit([['\n', 'world'], [' comma'], [' точка']]);
    renderChat(harness.client);
    const input = screen.getByRole('textbox', { name: 'Message' });
    fireEvent.change(input, { target: { value: 'Draft' } });
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    const node = FakeWorklet.latest;
    node?.emit(Int16Array.from([1]));
    await waitFor(() => expect(input).toHaveValue('Draft\nworld'));
    node?.emit(Int16Array.from([2]));
    await waitFor(() => expect(input).toHaveValue('Draft\nworld,'));
    node?.emit(Int16Array.from([3]));
    await waitFor(() => expect(input).toHaveValue('Draft\nworld,.'));
  });

  it('keeps later line breaks and the split-command limitation', async () => {
    const harness = await dictate([
      ['Hello', ' new line', ' world'],
      ['Привет', ' новая строка', 'мир'],
      ['new', 'line'],
      ['Try again.', 'Try again.'],
      [],
      ['again'],
    ]);
    const input = screen.getByRole('textbox', { name: 'Message' });
    const node = FakeWorklet.latest;
    node?.emit(Int16Array.from([1]));
    await waitFor(() => expect(input).toHaveValue('Hello\nworld'));
    node?.emit(Int16Array.from([2]));
    await waitFor(() => expect(input).toHaveValue('Hello\nworld Привет\nмир'));
    node?.emit(Int16Array.from([3]));
    await waitFor(() => expect(input).toHaveValue('Hello\nworld Привет\nмир new line'));
    node?.emit(Int16Array.from([4]));
    await waitFor(() => expect(input).toHaveValue(
      'Hello\nworld Привет\nмир new line Try again. Try again.',
    ));
    node?.emit(Int16Array.from([5]));
    node?.emit(Int16Array.from([6]));
    await waitFor(() => expect(input).toHaveValue(
      'Hello\nworld Привет\nмир new line Try again. Try again. again',
    ));
    expect(harness.languages).toEqual([['en']]);
  });

  it('sends the current composer language and the final stop text', async () => {
    installCapture();
    let releaseStop: (() => void) | undefined;
    const languages: string[][] = [];
    const client = fixtureClient({
      getVoiceInputRuntime: async () => runtime,
      startXaiVoiceInput: async (sessionId, spoken) => {
        languages.push(spoken);
        return { session_id: sessionId, stop_budget_ms: 20000 };
      },
      sendXaiVoiceAudio: async (sessionId) => ({ session_id: sessionId, pieces: ['Hello'] }),
      stopXaiVoiceInput: (sessionId) => new Promise((resolve) => {
        releaseStop = () => resolve({ session_id: sessionId, pieces: [' final'] });
      }),
    });
    const view = renderChat(client);
    const user = userEvent.setup();
    await user.click(screen.getByRole('button', { name: 'Latin to Russian transliteration' }));
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    expect(languages).toEqual([['ru']]);
    FakeWorklet.latest?.emit(Int16Array.from([1]));
    const input = screen.getByRole('textbox', { name: 'Message' });
    await waitFor(() => expect(input).toHaveValue('Hello'));
    fireEvent.click(screen.getByRole('button', { name: 'Send message' }));
    expect(view.props.onSubmitInput).not.toHaveBeenCalled();
    await waitFor(() => expect(releaseStop).toBeTypeOf('function'));
    await act(async () => { releaseStop?.(); });
    await waitFor(() => expect(view.props.onSubmitInput).toHaveBeenCalledWith('Hello final'));
  });

  it('reports a capture failure and releases the microphone on cancel', async () => {
    const { stopTrack } = installCapture();
    const client = fixtureClient({
      getVoiceInputRuntime: async () => runtime,
      sendXaiVoiceAudio: async () => { throw new Error('native bridge failed'); },
    });
    const view = renderChat(client);
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    FakeWorklet.latest?.emit(Int16Array.from([1]));
    expect(await screen.findByRole('alert')).toHaveTextContent(
      'Voice input stopped because transcription failed. Try again.',
    );
    expect(screen.getByRole('button', { name: 'Start voice input' })).toBeEnabled();
    expect(stopTrack).toHaveBeenCalled();
    expect(FakeContext.latest?.closed).toBe(true);

    stopTrack.mockClear();
    fireEvent.click(screen.getByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    view.rerenderVault('Projects');
    await waitFor(() => expect(stopTrack).toHaveBeenCalled());
    expect(FakeContext.latest?.closed).toBe(true);
  });

  it('reports an unsupported audio format', async () => {
    const { stopTrack } = installCapture();
    vi.stubGlobal('AudioContext', class extends FakeContext { sampleRate = 44100; });
    const start = vi.fn();
    renderChat(fixtureClient({
      getVoiceInputRuntime: async () => runtime,
      startXaiVoiceInput: start,
    }));
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    expect(await screen.findByRole('alert')).toHaveTextContent(
      'The audio format is unsupported.',
    );
    expect(screen.getByRole('button', { name: 'Start voice input' })).toBeEnabled();
    expect(start).not.toHaveBeenCalled();
    expect(stopTrack).toHaveBeenCalled();
    expect(FakeContext.latest?.closed).toBe(true);
  });
});
