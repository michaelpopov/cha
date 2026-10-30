import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { useState } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { ChaError, type AudioDownloadBatchAcceptance, type AudioDownloadBatchRequest, type ChaClient, type SessionSnapshot } from '../api/client';
import type { NativeBridge } from '../api/nativeBridge';
import type { SessionEventHandlers } from '../api/events';
import { initialAppState } from '../state/view';
import {
  bootstrapFixture,
  characterDetailFixture,
  fixtureClient,
  plainVoice,
  serifItalicVoice,
  snapshotFixture,
  voiceOutputRuntimeFixture,
} from '../test/fixtures';
import * as textFiles from '../textFile';
import { App } from './App';
import { ChatScreen, formatEntryTime } from './ChatScreen';

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
    },
  };
}

describe('live chat', () => {
  it('does not attach a pending dropped file after switching conversations', async () => {
    let finishRead!: (text: string) => void;
    vi.spyOn(textFiles, 'readTextFile').mockImplementation(() => new Promise((resolve) => { finishRead = resolve; }));
    const events = drivableEvents();
    const attachTextFile = vi.fn(async () => ({ clear_input: false }));
    render(<App client={fixtureClient({
      attachTextFile,
      getSessionSnapshot: async (forumId) => forumId === 'lobby' ? {
        ...snapshotFixture, forum: bootstrapFixture.forums[1], session_id: 'planning',
      } : snapshotFixture,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events);
    const file = new File(['File text'], 'plan.md');
    fireEvent.drop(screen.getByRole('textbox', { name: 'Message' }), {
      dataTransfer: { types: ['Files'], files: [file] },
    });
    expect(textFiles.readTextFile).toHaveBeenCalledTimes(1);
    fireEvent.click(screen.getByRole('button', { name: 'PlanningThe Lobby' }));
    await waitFor(() => expect(events.connections.some(({ key }) => key === 'lobby/planning')).toBe(true));
    await act(async () => finishRead('File text'));
    expect(attachTextFile).not.toHaveBeenCalled();
  });

  it('attaches dropped text without upload controls, keeps the draft, and rejects binary files', async () => {
    const events = drivableEvents();
    const attachTextFile = vi.fn(async () => ({ clear_input: false }));
    const submitInput = vi.fn(async () => ({ clear_input: true }));
    render(<App client={fixtureClient({
      attachTextFile, submitInput,
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events);
    expect(screen.queryByRole('button', { name: 'Attach text file' })).not.toBeInTheDocument();
    expect(screen.queryByLabelText('Text file')).not.toBeInTheDocument();
    const draft = screen.getByRole('textbox', { name: 'Message' });
    fireEvent.change(draft, { target: { value: 'Keep my question' } });
    function addFile(file: File) {
      fireEvent.drop(draft, { dataTransfer: { types: ['Files'], files: [file] } });
    }
    addFile(new File(['# Plan\n\nПривет'], 'plan.md'));
    await waitFor(() => expect(attachTextFile).toHaveBeenCalledWith(
      'entrance', 'welcome', { name: 'plan.md', text: '# Plan\n\nПривет' },
    ));
    expect(draft).toHaveValue('Keep my question');
    expect(submitInput).not.toHaveBeenCalled();
    await waitFor(() => expect(screen.getByRole('button', { name: 'Send message' })).toBeEnabled());
    addFile(new File(['a\0b'], 'binary.txt'));
    expect(await screen.findByRole('alert')).toHaveTextContent('Choose a UTF-8 text file.');
    expect(attachTextFile).toHaveBeenCalledTimes(1);
  });

  it('blocks file drops during generation and while another upload is pending', async () => {
    const events = drivableEvents();
    let finishUpload!: (result: { clear_input: boolean }) => void;
    const attachTextFile = vi.fn(() => new Promise<{ clear_input: boolean }>((resolve) => {
      finishUpload = resolve;
    }));
    render(<App client={fixtureClient({ attachTextFile })} connectSessionEvents={events.connect} />);
    await attachInitial(events, transcriptSnapshot());
    const input = screen.getByRole('textbox', { name: 'Message' });
    const dataTransfer = { types: ['Files'], files: [new File(['text'], 'notes.txt')], dropEffect: 'copy' };
    fireEvent.dragOver(input, { dataTransfer });
    expect(dataTransfer.dropEffect).toBe('none');
    fireEvent.drop(input, { dataTransfer });
    expect(attachTextFile).not.toHaveBeenCalled();

    act(() => events.handlers[0].onSnapshot(snapshotFixture));
    fireEvent.drop(input, { dataTransfer });
    await waitFor(() => expect(attachTextFile).toHaveBeenCalledTimes(1));
    fireEvent.dragOver(input, { dataTransfer });
    expect(dataTransfer.dropEffect).toBe('none');
    fireEvent.drop(input, { dataTransfer });
    expect(attachTextFile).toHaveBeenCalledTimes(1);
    await act(async () => finishUpload({ clear_input: false }));
    fireEvent.dragOver(input, { dataTransfer });
    expect(dataTransfer.dropEffect).toBe('copy');
  });

  it('opens and attaches the initial conversation on plain startup and Return to start', async () => {
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

    fireEvent.click(within(screen.getByLabelText('Sidebar')).getByRole('button', { name: 'Settings' }));
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

  it('shows preparation until the answer starts and never renders reasoning', async () => {
    const events = drivableEvents();
    const snapshot = transcriptSnapshot();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshot,
      transcript: [],
      generation: {
        ...snapshot.generation, phase: 'waiting', character_display_name: '',
      },
    });

    expect(within(screen.getByLabelText('Conversation transcript')).queryByText('Welcome'))
      .not.toBeInTheDocument();
    expect(screen.getByText('Preparing a response…')).toHaveAttribute('aria-live', 'polite');
    expect(screen.getByRole('button', { name: 'Stop generation' })).toBeEnabled();

    act(() => events.handlers[0].onSnapshot({
      ...snapshot,
      transcript: [],
      generation: { ...snapshot.generation, phase: 'waiting' },
    }));
    expect(screen.getByText('Assistant is preparing a response…')).toBeInTheDocument();

    act(() => events.handlers[0].onSnapshot({ ...snapshot, transcript: [] }));
    expect(screen.getByText('Assistant is preparing a response…')).toBeInTheDocument();
    expect(screen.getByLabelText('Conversation transcript')).not.toHaveTextContent('Checking');

    act(() => events.handlers[0].onSnapshot({
      ...snapshot,
      generation: { ...snapshot.generation, phase: 'answering' },
    }));
    expect(screen.queryByText(/preparing a response…/i)).not.toBeInTheDocument();
    expect(screen.getByText('Still here')).toBeInTheDocument();

    act(() => events.handlers[0].onSnapshot({
      ...snapshot,
      generation: { ...snapshot.generation, phase: 'stopping' },
    }));
    expect(screen.getByText('Stopping Assistant…')).toBeInTheDocument();
    expect(screen.queryByText(/preparing a response…/i)).not.toBeInTheDocument();

    act(() => events.handlers[0].onSnapshot({
      ...snapshot,
      generation: snapshotFixture.generation,
      transcript: [{ ...snapshot.transcript[0], status: 'complete' }],
    }));
    expect(screen.queryByText(/preparing a response…/i)).not.toBeInTheDocument();
    expect(screen.queryByText('Stopping Assistant…')).not.toBeInTheDocument();
    expect(screen.getByText('Still here')).toBeInTheDocument();
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

  it('shows each response context usage below that response', async () => {
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: [
        {
          id: 1, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
          addressed_to: '', addressed_to_name: '', text: 'First answer', status: 'complete',
          created_at: 1_700_000_000, input_tokens: 1_200, output_tokens: 300,
        },
        {
          id: 2, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
          addressed_to: '', addressed_to_name: '', text: 'Second answer', status: 'complete',
          created_at: 1_700_000_001, input_tokens: 50_000, output_tokens: 4_000,
        },
      ],
    });

    const totals = Array.from(document.querySelectorAll('.cha-message-tokens'));
    expect(totals.map((item) => item.textContent)).toEqual(['2K', '54K']);
    expect(totals[1].getAttribute('title')).toBe('50,000 input + 4,000 output tokens for this response');
  });

  it('marks only responses whose requests included web source data', async () => {
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: [true, false, undefined].map((web_search_used, index) => ({
        id: index + 1, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
        addressed_to: '', addressed_to_name: '', text: `Answer ${index + 1}`,
        status: 'complete', created_at: null, web_search_used,
      })),
    });

    const indicator = screen.getByRole('img', { name: 'Request included web source data' });
    expect(indicator).toHaveAttribute('title', 'Request included web source data');
    expect(indicator.closest('article')).toHaveTextContent('Answer 1');
    expect(document.querySelectorAll('.cha-message-web-search')).toHaveLength(1);
    expect(indicator.closest('.cha-message-meta')).not.toBeNull();
  });

  it('copies requests and visible response text to the clipboard', async () => {
    const writeText = vi.fn(async () => undefined);
    vi.stubGlobal('navigator', { ...navigator, clipboard: { writeText } });
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
          text: '[2026-01-02T03:04:05Z] **Answer**', status: 'complete', created_at: 1_700_000_001,
        },
      ],
    });

    fireEvent.click(screen.getByRole('button', { name: 'Copy your prompt' }));
    await waitFor(() => expect(writeText).toHaveBeenCalledWith('Question'));
    fireEvent.click(screen.getByRole('button', { name: "Copy Assistant's response" }));
    await waitFor(() => expect(writeText).toHaveBeenCalledWith('**Answer**'));
    expect(await screen.findAllByRole('button', { name: 'Copied to clipboard' }))
      .toHaveLength(2);
  });

  it('uses each stored prompt author\'s style without offering voice output', async () => {
    const startAudioDownload = vi.fn();
    const events = drivableEvents();
    const bootstrap = {
      ...bootstrapFixture,
      personas: bootstrapFixture.personas.map((persona) => persona.id === 'reader'
        ? { ...persona, appearance: serifItalicVoice }
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
    expect(screen.queryByRole('button', { name: /audio for your prompt/ })).not.toBeInTheDocument();
    expect(startAudioDownload).not.toHaveBeenCalled();
  });

  it('submits new replies in one batch while prompts remain available and serializes later batches', async () => {
    let complete!: (accepted: AudioDownloadBatchAcceptance) => void;
    const jobs: Array<{ entry_id: number; state: 'queued' }> = [];
    const startAudioDownload = vi.fn();
    const startAudioDownloadBatch = vi.fn((_forum: string, _session: string, request: AudioDownloadBatchRequest) => {
      if (request.entries.length === 3) return new Promise<AudioDownloadBatchAcceptance>((resolve) => { complete = resolve; });
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
    const snapshot = { ...snapshotFixture, transcript: Array.from({ length: 3 }, (_, index) =>
      ({ ...entry, id: index + 1, text: `Answer ${index + 1}` })) };
    await attachInitial(events, { ...snapshot, transcript: [] });
    const toggle = await screen.findByRole('button', { name: 'Auto audio response' });
    await waitFor(() => expect(toggle).toBeEnabled());
    fireEvent.click(toggle);
    act(() => events.handlers[0].onSnapshot(snapshot));
    await waitFor(() => expect(startAudioDownloadBatch).toHaveBeenCalledOnce());
    expect(startAudioDownloadBatch.mock.calls[0][2].entries).toHaveLength(3);
    expect(startAudioDownloadBatch.mock.calls[0][2].entries).toEqual([
      { entry_id: 1 }, { entry_id: 2 }, { entry_id: 3 },
    ]);
    expect(startAudioDownload).not.toHaveBeenCalled();
    fireEvent.change(screen.getByRole('textbox', { name: 'Message' }), { target: { value: 'Another prompt' } });
    fireEvent.click(screen.getByRole('button', { name: 'Send message' }));
    await waitFor(() => expect(submitInput).toHaveBeenCalledWith('entrance', 'welcome', { text: 'Another prompt' }));
    const updated = { ...snapshot, transcript: [...snapshot.transcript, { ...entry, id: 4, text: 'New answer' }] };
    act(() => events.handlers[0].onSnapshot(updated));
    expect(startAudioDownloadBatch).toHaveBeenCalledOnce();
    await act(async () => {
      jobs.push(...snapshot.transcript.map(({ id }) => ({ entry_id: id, state: 'queued' as const })));
      complete({ entries: jobs.map((job) => ({ ...job, cached: false })) });
    });
    await waitFor(() => expect(startAudioDownloadBatch).toHaveBeenCalledTimes(2));
    expect(startAudioDownloadBatch.mock.lastCall?.[2].entries.map((item) => item.entry_id)).toEqual([4]);
    act(() => events.handlers[0].onSnapshot({ ...updated, transcript: [...updated.transcript] }));
    expect(startAudioDownloadBatch).toHaveBeenCalledTimes(2);
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

  it('deletes an error report with the prompt that generated it', async () => {
    const deleteTurn = vi.fn(async () => ({ clear_input: false }));
    const events = drivableEvents();
    render(<App
      client={fixtureClient({ deleteTurn })}
      connectSessionEvents={events.connect}
    />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: [
        {
          id: 1, kind: 'human', participant_id: 'guest', display_name: 'Guest',
          addressed_to: 'assistant', addressed_to_name: 'Assistant',
          text: 'Question', status: 'complete', request_id: 5, created_at: 1_700_000_000,
        },
        {
          id: 2, kind: 'error', participant_id: 'assistant', display_name: 'Error',
          addressed_to: '', addressed_to_name: '',
          text: 'Provider rate limit exceeded.', status: 'failed', request_id: 5,
          created_at: 1_700_000_001,
        },
      ],
    });

    fireEvent.click(screen.getByRole('button', { name: 'Delete error report and its prompt' }));
    const dialog = within(await screen.findByRole('dialog'));
    expect(dialog.getByText(/Delete this error report and the prompt/)).toBeInTheDocument();
    fireEvent.click(dialog.getByRole('button', { name: 'Delete error report' }));
    await waitFor(() => expect(deleteTurn).toHaveBeenCalledWith(
      'entrance', 'welcome', { response_entry_id: 2 },
    ));

    act(() => events.handlers[0].onSnapshot({ ...snapshotFixture, transcript: [] }));
    expect(screen.queryByText('Question')).not.toBeInTheDocument();
    expect(screen.queryByText('Provider rate limit exceeded.')).not.toBeInTheDocument();
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
    const boundaryResponse = screen.getByText('Partial answer').closest<HTMLElement>('.cha-message');
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

    const toggle = screen.getByRole('button', { name: 'Auto audio response' });
    await waitFor(() => expect(toggle).toBeEnabled());
    fireEvent.click(toggle);
    expect(startAudioDownload).not.toHaveBeenCalled();
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

  it('hides leading blank lines in a response but keeps its paragraph breaks', async () => {
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshotFixture,
      transcript: [{
        id: 1, kind: 'character', participant_id: 'assistant', display_name: 'Assistant',
        addressed_to: '', addressed_to_name: '',
        text: '\n  \r\nFirst paragraph\n\nSecond paragraph',
        status: 'complete', created_at: 1_787_120_306,
      }],
    });

    expect(Array.from(document.querySelectorAll('.cha-message-text p'), (p) => p.textContent))
      .toEqual(['First paragraph', 'Second paragraph']);
  });

  it('renders model Markdown as safe HTML and updates it while streaming', async () => {
    const events = drivableEvents();
    const snapshot = transcriptSnapshot();
    const entry = { ...snapshot.transcript[0], text: '# Answer\n\nA **partial' };
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events, {
      ...snapshot,
      transcript: [
        { ...entry, id: 3, kind: 'human', text: '**Keep my prompt literal**', status: 'complete' },
        entry,
      ],
    });

    expect(screen.getByRole('heading', { name: 'Answer' })).toBeInTheDocument();
    expect(screen.getByText('A **partial')).toBeInTheDocument();
    expect(screen.getByText('**Keep my prompt literal**')).toBeInTheDocument();

    act(() => events.handlers[0].onSnapshot({
      ...snapshot,
      transcript: [{
        ...entry,
        text: '# Answer\n\nA **complete** answer.\n\n- One\n- Two\n\n```js\nconst x = 1;\n```\n\n'
          + '<img src="https://example.test/tracker" onerror="alert(1)">\n\n'
          + '| Machine | Memory |\n| :--- | ---: |\n| Framework Desktop | 64 GB |',
      }],
    }));

    expect(screen.getByText('complete').tagName).toBe('STRONG');
    expect(screen.getAllByRole('listitem').map((item) => item.textContent)).toEqual(['One', 'Two']);
    expect(screen.getByText('const x = 1;').parentElement?.tagName).toBe('PRE');
    expect(screen.getByRole('table')).toBeInTheDocument();
    expect(screen.getByRole('columnheader', { name: 'Memory' })).toHaveAttribute('align', 'right');
    expect(screen.getByRole('cell', { name: '64 GB' })).toHaveAttribute('align', 'right');
    expect(document.querySelector('.cha-message-text img')).toBeNull();
    expect(document.querySelector('.cha-message-text [onerror]')).toBeNull();
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

  it('uses the backend all-characters target and preserves explicit addressing', async () => {
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
    expect(setDefaultCharacter).toHaveBeenCalledWith('entrance', 'welcome', '*');
    act(() => events.handlers[0].onSnapshot({ ...snapshot, default_character_id: '*' }));
    expect(chooser).toHaveValue('*');
    expect(input).toHaveAttribute('placeholder', 'Message all characters');
    expect(screen.getByLabelText('Current chat context')).toHaveTextContent('To: All characters');

    await user.type(input, 'Shared question');
    await user.click(screen.getByRole('button', { name: 'Send message' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('not accepted');
    expect(submitInput).toHaveBeenLastCalledWith(
      'entrance', 'welcome', { text: 'Shared question' },
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
      'entrance', 'welcome', { text: '@Guide is part of the question' },
    );

    fireEvent.click(within(screen.getByLabelText('Sidebar')).getByRole('button', { name: 'Settings' }));
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

  it('keeps the parent draft when the snapshot session changes without remounting', async () => {
    const user = userEvent.setup();
    const onSubmitInput = vi.fn(async () => ({ clear_input: true }));
    const client = fixtureClient();
    function StayMounted({ snapshot }: { snapshot: SessionSnapshot }) {
      const [draft, setDraft] = useState('');
      return (
        <ChatScreen
          client={client}
          dispatch={vi.fn()} navigate={vi.fn()}
          draft={draft}
          onDraftChange={setDraft}
          onCoverConversation={vi.fn()}
          onDeleteTurn={vi.fn()}
          onRetryStream={vi.fn()}
          onReturnToStart={vi.fn()}
          onSetCharacterSettings={vi.fn()} onSetDefaultCharacter={vi.fn()}
          onStopGeneration={vi.fn()}
          onSubmitInput={onSubmitInput}
          onAttachTextFile={vi.fn()}
          onUncoverConversation={vi.fn()}
          playbackPositions={new Map()}
          state={{
            ...initialAppState,
            bootstrap: bootstrapFixture,
            sessionSnapshot: snapshot,
            streamStatus: 'connected',
            currentDefaultCharacterId: snapshot.default_character_id,
          }}
        />
      );
    }
    const { rerender } = render(<StayMounted snapshot={snapshotFixture} />);
    const input = screen.getByRole('textbox', { name: 'Message' });
    await user.type(input, 'Only for Welcome');

    rerender(<StayMounted snapshot={{ ...snapshotFixture, session_label: 'Renamed' }} />);
    expect(input).toHaveValue('Only for Welcome');

    rerender(<StayMounted snapshot={{ ...snapshotFixture, session_id: 'another-session' }} />);
    expect(screen.getByRole('textbox', { name: 'Message' })).toBe(input);
    expect(input).toHaveValue('Only for Welcome');
    await user.click(screen.getByRole('button', { name: 'Send message' }));
    expect(onSubmitInput).toHaveBeenCalledExactlyOnceWith('Only for Welcome');
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

    await user.click(recent.getByRole('button', { name: /Welcome/ }));
    await waitFor(() => expect(events.connections.at(-1)?.key).toBe('entrance/welcome'));
    const welcome = events.handlers.at(-1);
    act(() => welcome?.onSnapshot(snapshotFixture));
    const welcomeInput = await screen.findByRole('textbox', { name: 'Message' });
    await waitFor(() => expect(welcomeInput).toBeEnabled());
    expect(welcomeInput).toHaveValue('');
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

    expect(screen.getByText('A considered answer').closest('.cha-message-text')).toHaveClass(
      'cha-message-text', 'cha-font-serif', 'cha-slant-italic', 'cha-weight-semibold',
      'cha-scale-large', 'cha-color-accent',
    );
    expect(screen.getByText('A considered answer').closest('.cha-message-text')).not.toHaveClass('cha-weight-bold');
    expect(screen.getByText('A plain answer').closest('.cha-message-text')?.className).toBe('cha-message-text');
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
    expect(screen.getByRole('button', { name: 'Return to start' })).toBeInTheDocument();
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
    expect(screen.queryByRole('button', { name: 'Return to start' })).not.toBeInTheDocument();
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
    act(() => events.handlers[0].onError());
    await waitFor(() => expect(events.connections).toHaveLength(2));
    act(() => events.handlers[1].onError());

    // A failed replacement while the last snapshot still says `reloading`
    // must not withhold the only way back.
    expect(await screen.findByRole('alert')).toHaveTextContent(
      'Live updates could not be restored.',
    );
    expect(screen.getByRole('button', { name: 'Retry' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Return to start' })).toBeInTheDocument();
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

    act(() => events.handlers[0].onError());
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
  it('stops after one failed replacement with an explicit Retry action', async () => {
    const events = drivableEvents();
    render(
      <App
        client={fixtureClient()}
        connectSessionEvents={events.connect}
      />,
    );
    await attachInitial(events);
    act(() => events.handlers[0].onError());
    await waitFor(() => expect(events.connections).toHaveLength(2));
    act(() => events.handlers[1].onError());

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

  it('offers Retry and Browse sessions after the session-limit bound is exhausted', async () => {
    window.history.replaceState(null, '', '/#/s/lobby/planning/');
    const openSession = vi.fn(async () => { throw capacityError(); });
    const client = fixtureClient({ openSession });
    render(<App client={client} retryDelays={[0, 0]} />);

    expect(await screen.findByRole('heading', { name: 'Session unavailable' })).toBeInTheDocument();
    expect(screen.getByRole('alert')).toHaveTextContent('Another session has not closed yet');
    expect(screen.getByRole('button', { name: 'Retry' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Browse sessions' })).toBeInTheDocument();
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

    await waitFor(() => expect(within(screen.getByLabelText('Sidebar')).getByRole('button', { name: 'Settings' })).toBeEnabled());
    fireEvent.click(within(screen.getByLabelText('Sidebar')).getByRole('button', { name: 'Settings' }));
    await user.click(await screen.findByRole('button', { name: 'Forums' }));
    await user.click(screen.getByRole('button', { name: 'The LobbyGuide' }));
    await user.click(await screen.findByRole('button', { name: /New session/ }));

    expect(await screen.findByRole('alert')).toHaveTextContent('Another session has not closed yet');
    expect(screen.getByRole('heading', { name: 'Session unavailable' })).toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Retry' }));

    await waitFor(() => expect(events.connections.some(({ key }) => key === 'lobby/created')).toBe(true));
    expect(createSession).toHaveBeenCalledTimes(1);
    expect(openSession.mock.calls.filter(([, id]) => id === 'created')).toHaveLength(3);
  });
});

describe('pending recipient detection', () => {
  for (const sendFirst of [true, false]) {
    it(`allows Stop during Send and preserves independent flags (${sendFirst ? 'send' : 'stop'} resolves first)`, async () => {
      const user = userEvent.setup();
      const events = drivableEvents();
      let finishSend!: (value: { clear_input: boolean }) => void;
      let finishStop!: (value: { clear_input: boolean }) => void;
      const submitInput = vi.fn(() => new Promise<{ clear_input: boolean }>((resolve) => { finishSend = resolve; }));
      const stopGeneration = vi.fn(() => new Promise<{ clear_input: boolean }>((resolve) => { finishStop = resolve; }));
      render(<App client={fixtureClient({ submitInput, stopGeneration })} connectSessionEvents={events.connect} />);
      await attachInitial(events);
      const input = screen.getByRole('textbox', { name: 'Message' });
      await user.type(input, 'Original');
      await user.click(screen.getByRole('button', { name: 'Send message' }));
      act(() => events.handlers[0].onSnapshot({ ...snapshotFixture,
        generation: { ...snapshotFixture.generation, active: true, phase: 'waiting', character_id: '', character_display_name: '', request_id: undefined },
      }));
      expect(screen.getByRole('button', { name: 'Stop generation' })).toBeEnabled();
      expect(screen.getByRole('combobox', { name: 'Choose message target' })).toBeDisabled();
      await user.type(input, ' appended');
      await user.click(screen.getByRole('button', { name: 'Stop generation' }));
      expect(stopGeneration).toHaveBeenCalledOnce();
      act(() => events.handlers[0].onSnapshot(snapshotFixture));
      await act(async () => { (sendFirst ? finishSend : finishStop)({ clear_input: false }); });
      expect(screen.getByRole('button', { name: 'Send message' })).toBeDisabled();
      expect(screen.getByRole('combobox', { name: 'Choose message target' })).toBeDisabled();
      await act(async () => { (sendFirst ? finishStop : finishSend)({ clear_input: false }); });
      expect(screen.getByRole('button', { name: 'Send message' })).toBeEnabled();
      expect(input).toHaveValue('Original appended');
    });
  }

  for (const replace of [false, true]) {
    it(`accepted detection preserves ${replace ? 'a replaced draft' : 'an appended suffix'}`, async () => {
      const user = userEvent.setup();
      const events = drivableEvents();
      let finish!: (value: { clear_input: boolean }) => void;
      render(<App client={fixtureClient({ submitInput: () => new Promise((resolve) => { finish = resolve; }) })} connectSessionEvents={events.connect} />);
      await attachInitial(events);
      const input = screen.getByRole('textbox', { name: 'Message' });
      await user.type(input, 'Original');
      await user.click(screen.getByRole('button', { name: 'Send message' }));
      if (replace) await user.clear(input);
      await user.type(input, ' More text');
      await act(async () => { finish({ clear_input: true }); });
      expect(input).toHaveValue(replace ? ' More text' : 'More text');
    });
  }

  it('reports a departed composer failure in App and leaves the new draft intact', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    let rejectSend!: (reason: unknown) => void;
    render(<App client={fixtureClient({ submitInput: () => new Promise((_, reject) => { rejectSend = reject; }) })} connectSessionEvents={events.connect} />);
    await attachInitial(events);
    await user.type(screen.getByRole('textbox', { name: 'Message' }), 'Original');
    await user.click(screen.getByRole('button', { name: 'Send message' }));
    fireEvent.click(within(screen.getByLabelText('Sidebar')).getByRole('button', { name: 'Settings' }));
    await act(async () => { rejectSend(new ChaError('invalid_argument', 'Recipient was removed.')); });
    const error = await screen.findByRole('alert');
    expect(error).toHaveTextContent('Message to');
    expect(error).toHaveTextContent('Recipient was removed.');
    expect(error).toHaveTextContent('Welcome');
    await user.click(screen.getByRole('button', { name: 'Dismiss message error' }));
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });

  it('reports an old send failure after leaving and returning to the same conversation', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    let rejectSend!: (reason: unknown) => void;
    render(<App client={fixtureClient({ submitInput: () => new Promise((_, reject) => { rejectSend = reject; }) })} connectSessionEvents={events.connect} />);
    await attachInitial(events);
    await user.type(screen.getByRole('textbox', { name: 'Message' }), 'Original');
    await user.click(screen.getByRole('button', { name: 'Send message' }));
    await user.click(within(screen.getByLabelText('Sidebar')).getByRole('button', { name: 'Settings' }));
    await user.click(within(screen.getByLabelText('Recent sessions')).getByRole('button', { name: /Welcome/ }));
    const input = await screen.findByRole('textbox', { name: 'Message' });
    expect(input).toHaveValue('Original');
    await user.type(input, 'New draft');
    await act(async () => { rejectSend(new ChaError('invalid_argument', 'Recipient was removed.')); });
    expect(screen.getByRole('alert')).toHaveTextContent('Message to');
    expect(screen.getByRole('alert')).toHaveTextContent('Recipient was removed.');
    expect(input).toHaveValue('OriginalNew draft');
  });

});

describe('submission ownership after navigation', () => {
  for (const fails of [false, true]) {
    it(`a late ${fails ? 'failure' : 'success'} leaves the next conversation's draft and pending send alone`, async () => {
      const user = userEvent.setup();
      const events = drivableEvents();
      let finishOriginal!: (value: { clear_input: boolean }) => void;
      let failOriginal!: (reason: unknown) => void;
      let finishCurrent!: (value: { clear_input: boolean }) => void;
      const planning: SessionSnapshot = { ...snapshotFixture, forum: bootstrapFixture.forums[1], session_id: 'planning', session_label: 'Planning', characters: [bootstrapFixture.characters[1]], default_character_id: 'guide' };
      const submitInput = vi.fn((_forum: string, session: string) => new Promise<{ clear_input: boolean }>((resolve, reject) => {
        if (session === 'welcome') { finishOriginal = resolve; failOriginal = reject; }
        else finishCurrent = resolve;
      }));
      render(<App client={fixtureClient({ submitInput, getSessionSnapshot: async (forum) => forum === 'lobby' ? planning : snapshotFixture })} connectSessionEvents={events.connect} />);
      await attachInitial(events);
      await user.type(screen.getByRole('textbox', { name: 'Message' }), 'Original');
      await user.click(screen.getByRole('button', { name: 'Send message' }));
      await user.click(within(screen.getByLabelText('Recent sessions')).getByRole('button', { name: /^Planning/ }));
      await waitFor(() => expect(events.connections[1]?.key).toBe('lobby/planning'));
      act(() => events.handlers[1].onSnapshot(planning));
      const input = await screen.findByRole('textbox', { name: 'Message' });
      await user.type(input, 'Current draft');
      await user.click(screen.getByRole('button', { name: 'Send message' }));
      await act(async () => {
        if (fails) failOriginal(new ChaError('invalid_argument', 'Recipient was removed.'));
        else finishOriginal({ clear_input: true });
      });
      expect(input).toHaveValue('Current draft');
      expect(screen.getByRole('button', { name: 'Send message' })).toBeDisabled();
      if (fails) {
        expect(screen.getByRole('alert')).toHaveTextContent('Welcome');
        expect(screen.getByRole('alert')).toHaveTextContent('Recipient was removed.');
      } else expect(screen.queryByRole('alert')).not.toBeInTheDocument();
      await act(async () => { finishCurrent({ clear_input: true }); });
      expect(input).toHaveValue('');
    });
  }
});

function pictureClient(overrides: Partial<ChaClient> = {}) {
  return fixtureClient({
    getCharacter: async () => ({ ...characterDetailFixture, markdown_files: ['CHARACTER.md', 'PICTURE.png'] }),
    ...overrides,
  });
}

describe('chat pictures', () => {
  const portrait = { filename: 'PICTURE.png' as const, mime_type: 'image/png' as const, content_base64: 'original-bytes' };

  it.each(['missing', 'request', 'decode'])('keeps chat usable for a %s picture', async (failure) => {
    const events = drivableEvents();
    const getCharacterPicture = vi.fn().mockImplementation(() => failure === 'request'
      ? Promise.reject(new Error('read failed')) : Promise.resolve(failure === 'missing' ? null : portrait));
    render(<App client={pictureClient({ getCharacterPicture })} connectSessionEvents={events.connect} />);
    await attachInitial(events);
    act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('toggle-pictures'));
    if (failure === 'decode') fireEvent.error(await screen.findByRole('img', { name: 'Assistant' }));
    if (failure !== 'missing') expect(await screen.findByText('Picture unavailable')).toBeInTheDocument();
    else await waitFor(() => expect(getCharacterPicture).toHaveBeenCalled());
    expect(screen.queryByRole('img')).not.toBeInTheDocument();
    expect(screen.getByRole('textbox', { name: 'Message' })).toBeEnabled();
    expect(getCharacterPicture).toHaveBeenCalledTimes(1);
  });
});

const planningSnapshot: SessionSnapshot = {
  ...snapshotFixture,
  forum: bootstrapFixture.forums[1],
  session_id: 'planning',
  session_label: 'Planning',
  characters: [bootstrapFixture.characters[1]],
  default_character_id: 'guide',
};

function contextChangeSource() {
  let notify: ((payload: { causing_request_id?: number; state?: string }) => void) | null = null;
  const contextEvents: Pick<NativeBridge, 'on'> = {
    on(_event, handler) {
      notify = handler as typeof notify;
      return () => {
        if (notify === handler) notify = null;
      };
    },
  };
  return {
    contextEvents,
    emit() {
      notify?.({ causing_request_id: 1, state: 'running' });
    },
  };
}

describe('prompt draft ownership', () => {
  it('keeps a chat draft across the settings panel and restores the same conversation', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events);
    act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('toggle-pictures'));
    await user.type(screen.getByRole('textbox', { name: 'Message' }), 'Still here');
    await user.click(within(screen.getByLabelText('Sidebar')).getByRole('button', { name: 'Settings' }));
    await user.click(within(screen.getByLabelText('Recent sessions')).getByRole('button', { name: /Welcome/ }));
    const input = await screen.findByRole('textbox', { name: 'Message' });
    expect(input).toHaveValue('Still here');
  });

  it('keeps the chat draft when a different conversation fails to open', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    render(<App client={fixtureClient({
      openSession: async (forumId, sessionId) => {
        if (sessionId === 'planning') throw new ChaError('invalid_argument', 'That session is missing.');
        return { forum_id: forumId, session_id: sessionId };
      },
    })} connectSessionEvents={events.connect} />);
    await attachInitial(events);
    act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('toggle-pictures'));
    await user.type(screen.getByRole('textbox', { name: 'Message' }), 'Keep this');
    await user.click(within(screen.getByLabelText('Recent sessions')).getByRole('button', { name: /^Planning/ }));
    expect(await screen.findByRole('heading', { name: 'Session unavailable' })).toBeInTheDocument();
    expect(screen.getByText('That session is missing.')).toBeInTheDocument();

    await user.click(within(screen.getByLabelText('Recent sessions')).getByRole('button', { name: /Welcome/ }));
    const input = await screen.findByRole('textbox', { name: 'Message' });
    await waitFor(() => expect(input).toBeEnabled());
    expect(input).toHaveValue('Keep this');
  });

  it('keeps a chat draft through maintenance and shows it again in the same conversation', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    const maintenance = contextChangeSource();
    render(<App
      client={fixtureClient({
        getSessionSnapshot: async (forumId) => forumId === 'lobby' ? planningSnapshot : snapshotFixture,
      })}
      connectSessionEvents={events.connect}
      contextEvents={maintenance.contextEvents}
    />);
    await attachInitial(events);
    act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('toggle-pictures'));
    await user.click(within(screen.getByLabelText('Recent sessions')).getByRole('button', { name: /^Planning/ }));
    await waitFor(() => expect(events.connections.at(-1)?.key).toBe('lobby/planning'));
    act(() => events.handlers.at(-1)?.onSnapshot(planningSnapshot));
    const planningInput = await screen.findByRole('textbox', { name: 'Message' });
    await waitFor(() => expect(planningInput).toBeEnabled());
    await user.type(planningInput, 'Plan text');

    act(() => maintenance.emit());
    await waitFor(() => expect(events.connections.at(-1)?.key).toBe('entrance/welcome'));
    act(() => events.handlers.at(-1)?.onSnapshot(snapshotFixture));
    const welcomeInput = await screen.findByRole('textbox', { name: 'Message' });
    await waitFor(() => expect(welcomeInput).toBeEnabled());
    expect(welcomeInput).toHaveValue('');

    await user.click(within(screen.getByLabelText('Recent sessions')).getByRole('button', { name: /^Planning/ }));
    await waitFor(() => expect(events.connections.at(-1)?.key).toBe('lobby/planning'));
    act(() => events.handlers.at(-1)?.onSnapshot(planningSnapshot));
    const restored = await screen.findByRole('textbox', { name: 'Message' });
    await waitFor(() => expect(restored).toBeEnabled());
    expect(restored).toHaveValue('Plan text');
  });

  it('clears both drafts only after a vault switch succeeds', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    const switchVault = vi.fn(async () => undefined);
    render(<App
      client={fixtureClient({ switchVault })}
      connectSessionEvents={events.connect}
      contextEvents={contextChangeSource().contextEvents}
    />);
    await attachInitial(events);
    act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('toggle-pictures'));
    await user.type(screen.getByRole('textbox', { name: 'Message' }), 'Personal draft');
    await user.selectOptions(screen.getByLabelText('Vault'), 'Projects');
    await waitFor(() => expect(switchVault).toHaveBeenCalledWith('Projects', undefined));
    expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('');
  });

  it('keeps the draft when a vault switch fails or the password dialog is canceled', async () => {
    const user = userEvent.setup();
    const events = drivableEvents();
    let requirePassword = false;
    const switchVault = vi.fn(async () => {
      if (requirePassword) throw new ChaError('vault_password_required', 'Password required.');
      throw new ChaError('invalid_argument', 'The vault could not be switched.');
    });
    render(<App
      client={fixtureClient({ switchVault })}
      connectSessionEvents={events.connect}
    />);
    await attachInitial(events);
    act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('toggle-pictures'));
    const input = screen.getByRole('textbox', { name: 'Message' });
    await user.type(input, 'Personal draft');
    await user.selectOptions(screen.getByLabelText('Vault'), 'Projects');
    await waitFor(() => expect(switchVault).toHaveBeenCalledOnce());
    expect(input).toHaveValue('Personal draft');

    requirePassword = true;
    await user.selectOptions(screen.getByLabelText('Vault'), 'Projects');
    fireEvent.click(await screen.findByRole('button', { name: 'Cancel' }));
    expect(switchVault).toHaveBeenCalledTimes(2);
    expect(input).toHaveValue('Personal draft');
  });
});

describe('session character toolbar integration', () => {
  it('saves only the changed field and preserves confirmed values and the input draft', async () => {
    const events = drivableEvents();
    let finish!: (result: { clear_input: boolean }) => void;
    const setCharacterSettings = vi.fn(() => new Promise<{ clear_input: boolean }>((resolve) => { finish = resolve; }));
    render(<App client={fixtureClient({ setCharacterSettings })} connectSessionEvents={events.connect} />);
    expect(screen.queryByLabelText('Model')).toBeNull();
    await attachInitial(events);
    const input = screen.getByRole('textbox', { name: 'Message' });
    fireEvent.change(input, { target: { value: 'Keep this draft' } });
    fireEvent.change(screen.getByLabelText('Effort'), { target: { value: 'high' } });
    expect(setCharacterSettings).toHaveBeenCalledWith('entrance', 'welcome', {
      character_id: 'assistant', reasoning_effort: 'high',
    });
    expect(screen.getByLabelText('Effort')).toHaveValue('none');
    expect(screen.getByLabelText('Effort')).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Send message' })).toBeDisabled();
    act(() => events.handlers[0].onSnapshot({
      ...snapshotFixture,
      chat_settings: { ...snapshotFixture.chat_settings,
        characters: [{ character_id: 'assistant', provider_id: 'test', reasoning_effort: 'high' }] },
    }));
    await act(async () => finish({ clear_input: false }));
    expect(screen.getByLabelText('Effort')).toHaveValue('high');
    expect(screen.getByLabelText('Effort')).toBeEnabled();
    expect(input).toHaveValue('Keep this draft');
  });

  it('disables toolbar edits during generation, classification, reconnect and shutdown', async () => {
    const events = drivableEvents();
    render(<App client={fixtureClient()} connectSessionEvents={events.connect} />);
    await attachInitial(events);
    act(() => events.handlers[0].onSnapshot({
      ...snapshotFixture, generation: { ...snapshotFixture.generation, active: true },
    }));
    expect(screen.getByLabelText('Model')).toBeDisabled();
    expect(screen.getByLabelText('Effort')).toBeDisabled();
    act(() => events.handlers[0].onSnapshot(snapshotFixture));
    expect(screen.getByLabelText('Model')).toBeEnabled();
    act(() => events.handlers[0].onSnapshot({ ...snapshotFixture, lifecycle: 'stopping' }));
    expect(screen.getByLabelText('Model')).toBeDisabled();
  });
});
