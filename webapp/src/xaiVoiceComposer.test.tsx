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

class FakeWorklet extends EventTarget {
  static latest: FakeWorklet | null = null;

  readonly port = new FakePort();

  constructor(_context: unknown, _name: string) {
    super();
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
  const track = Object.assign(new EventTarget(), { kind: 'audio', readyState: 'live', stop: stopTrack });
  Object.defineProperty(navigator, 'mediaDevices', {
    configurable: true,
    value: {
      getUserMedia: vi.fn(async () => ({
        getAudioTracks: () => [track],
        getTracks: () => [track],
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
  it('shows interim words and replaces them when xAI revises the transcript', async () => {
    installCapture();
    const replies = [
      { pieces: [], preview: 'I want to build a' },
      { pieces: [], preview: 'I want to make an app' },
      { pieces: ['I want to make an app.'], preview: '' },
    ];
    const client = fixtureClient({
      getVoiceInputRuntime: async () => runtime,
      sendXaiVoiceAudio: async (sessionId) => ({
        session_id: sessionId, ...replies.shift()!,
      }),
    });
    renderChat(client);
    const input = screen.getByRole('textbox', { name: 'Message' });
    fireEvent.change(input, { target: { value: 'Draft' } });
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });

    FakeWorklet.latest?.emit(Int16Array.from([1]));
    await waitFor(() => expect(input).toHaveValue('Draft I want to build a'));
    FakeWorklet.latest?.emit(Int16Array.from([2]));
    await waitFor(() => expect(input).toHaveValue('Draft I want to make an app'));
    FakeWorklet.latest?.emit(Int16Array.from([3]));
    await waitFor(() => expect(input).toHaveValue('Draft I want to make an app.'));
  });

  it('keeps edits before the preview without duplicating dictated words', async () => {
    installCapture();
    const replies = [
      { pieces: [], preview: 'I want to build a' },
      { pieces: [], preview: 'I want to build a small app' },
      { pieces: ['I want to build a small app.'], preview: '' },
    ];
    renderChat(fixtureClient({
      getVoiceInputRuntime: async () => runtime,
      sendXaiVoiceAudio: async (sessionId) => ({
        session_id: sessionId, ...replies.shift()!,
      }),
    }));
    const input = screen.getByRole('textbox', { name: 'Message' });
    fireEvent.change(input, { target: { value: 'Helo' } });
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });

    FakeWorklet.latest?.emit(Int16Array.from([1]));
    await waitFor(() => expect(input).toHaveValue('Helo I want to build a'));
    fireEvent.change(input, { target: { value: 'Hello I want to build a' } });
    FakeWorklet.latest?.emit(Int16Array.from([2]));
    await waitFor(() => expect(input).toHaveValue('Hello I want to build a small app'));
    FakeWorklet.latest?.emit(Int16Array.from([3]));
    await waitFor(() => expect(input).toHaveValue('Hello I want to build a small app.'));
  });

  it('removes unconfirmed preview text when the vault changes', async () => {
    installCapture();
    const view = renderChat(fixtureClient({
      getVoiceInputRuntime: async () => runtime,
      sendXaiVoiceAudio: async (sessionId) => ({
        session_id: sessionId, pieces: [], preview: 'maybe wrong',
      }),
    }));
    const input = screen.getByRole('textbox', { name: 'Message' });
    fireEvent.change(input, { target: { value: 'Draft' } });
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    FakeWorklet.latest?.emit(Int16Array.from([1]));
    await waitFor(() => expect(input).toHaveValue('Draft maybe wrong'));

    view.rerenderVault('Projects');
    await waitFor(() => expect(input).toHaveValue('Draft'));
    expect(screen.getByRole('button', { name: 'Start voice input' })).toBeEnabled();
  });

  it('removes unconfirmed preview text when transcription fails', async () => {
    installCapture();
    let calls = 0;
    renderChat(fixtureClient({
      getVoiceInputRuntime: async () => runtime,
      sendXaiVoiceAudio: async (sessionId) => {
        calls += 1;
        if (calls === 1) {
          return { session_id: sessionId, pieces: [], preview: 'maybe wrong' };
        }
        throw new Error('native bridge failed');
      },
    }));
    const input = screen.getByRole('textbox', { name: 'Message' });
    fireEvent.change(input, { target: { value: 'Draft' } });
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    FakeWorklet.latest?.emit(Int16Array.from([1]));
    await waitFor(() => expect(input).toHaveValue('Draft maybe wrong'));
    FakeWorklet.latest?.emit(Int16Array.from([2]));

    expect(await screen.findByRole('alert')).toHaveTextContent(
      'Voice input stopped because transcription failed. Try again.',
    );
    expect(input).toHaveValue('Draft');
  });

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

  it('starts a new dictation after failure and does not replay audio', async () => {
    installCapture();
    const audio: string[] = [];
    let cancels = 0;
    const client = fixtureClient({
      getVoiceInputRuntime: async () => runtime,
      sendXaiVoiceAudio: async (sessionId, pcm) => {
        audio.push(pcm);
        if (audio.length === 1) throw new Error('native bridge failed');
        return { session_id: sessionId, pieces: ['Second'] };
      },
      cancelXaiVoiceInput: async () => { cancels += 1; },
    });
    renderChat(client);
    const input = screen.getByRole('textbox', { name: 'Message' });
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    FakeWorklet.latest?.emit(Int16Array.from([1]));
    expect(await screen.findByRole('alert')).toHaveTextContent(
      'Voice input stopped because transcription failed. Try again.',
    );
    expect(input).toHaveValue('');
    expect(audio).toHaveLength(1);
    expect(cancels).toBe(1);

    fireEvent.click(screen.getByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    expect(audio).toHaveLength(1);
    FakeWorklet.latest?.emit(Int16Array.from([2, 3]));
    await waitFor(() => expect(input).toHaveValue('Second'));
    expect(audio).toHaveLength(2);
    expect(audio[1]).not.toBe(audio[0]);
    expect(cancels).toBe(1);
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });
});
