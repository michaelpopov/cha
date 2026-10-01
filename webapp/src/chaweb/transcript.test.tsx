import { fireEvent, render, screen } from '@testing-library/react';
import { useState } from 'react';
import { expect, it, vi } from 'vitest';

import { serifItalicVoice } from '../test/fixtures';
import { Conversation } from './conversation';
import { Transcript, type TranscriptEntry } from './transcript';

function entry(overrides: Partial<TranscriptEntry> = {}): TranscriptEntry {
  return {
    id: 1,
    kind: 'character',
    participant_id: 'guide',
    display_name: 'Guide',
    addressed_to: '',
    addressed_to_name: '',
    text: 'Hello',
    status: 'complete',
    created_at: 1_700_000_000,
    ...overrides,
  };
}

function mockScroll(element: HTMLElement, scrollHeight: number, clientHeight: number) {
  let scrollTop = 0;
  Object.defineProperty(element, 'scrollHeight', {
    configurable: true, get: () => scrollHeight,
  });
  Object.defineProperty(element, 'clientHeight', {
    configurable: true, get: () => clientHeight,
  });
  Object.defineProperty(element, 'scrollTop', {
    configurable: true,
    get: () => scrollTop,
    set: (value: number) => { scrollTop = value; },
  });
  return {
    get scrollTop() { return scrollTop; },
    set scrollTop(value: number) { scrollTop = value; },
  };
}

it('offers Read aloud only for completed character replies and keeps cached audio playable', () => {
  const toggle = vi.fn();
  const speech = { available: true, entryId: null, state: 'loading' as const, error: null, toggle,
    clearing: false, clearDisabled: false, clear: vi.fn(async () => undefined) };
  const props = {
    characters: [], personas: [], layoutKey: 'compact', sessionKey: 'lobby/planning', speech,
    entries: [entry({ id: 1 }), entry({ id: 2, status: 'streaming' }),
      entry({ id: 3, kind: 'human' }), entry({ id: 4, text: ' ' }),
      entry({ id: 5, has_cached_audio: true })],
  };
  const { rerender } = render(<Transcript {...props} />);
  expect(screen.getAllByRole('button', { name: 'Read aloud' })).toHaveLength(2);
  fireEvent.click(screen.getAllByRole('button', { name: 'Read aloud' })[0]!);
  expect(toggle).toHaveBeenCalledWith(props.entries[0]);
  rerender(<Transcript {...props} speech={{ ...speech, entryId: 1 }} />);
  expect(screen.getByRole('button', { name: 'Stop audio' })).toHaveTextContent('loading');
  rerender(<Transcript {...props} speech={{ ...speech, entryId: 1, state: 'playing' }} />);
  fireEvent.click(screen.getByRole('button', { name: 'Pause audio' }));
  expect(toggle).toHaveBeenLastCalledWith(props.entries[0]);
  rerender(<Transcript {...props} speech={{ ...speech, available: false }} />);
  expect(screen.getAllByRole('button', { name: 'Read aloud' })).toHaveLength(1);
  rerender(<Transcript {...props} speech={{ ...speech, clearing: true }} />);
  screen.getAllByRole('button', { name: 'Read aloud' }).forEach((button) => expect(button).toBeDisabled());
  rerender(<Transcript {...props} deleting />);
  screen.getAllByRole('button', { name: 'Read aloud' }).forEach((button) => expect(button).toBeDisabled());
});

it('renders stored replies, sanitizes Markdown, and keeps a name the roster no longer has', () => {
  render(
    <Transcript
      characters={[{ id: 'guide', appearance: serifItalicVoice }]}
      entries={[
        entry({ id: 1, text: 'See **this**\n\n<script>alert(1)</script>\n\n```\ncode\n```' }),
        entry({
          id: 2,
          participant_id: 'retired',
          display_name: 'Retired',
          text: 'Still mine',
          status: 'streaming',
        }),
        entry({ id: 3, kind: 'human', participant_id: 'reader', display_name: 'Reader', text: 'Mine', status: 'cancelled' }),
        entry({ id: 4, kind: 'error', display_name: 'Notice', text: 'Broken', status: 'failed', created_at: null }),
      ]}
      layoutKey="compact:700"
      personas={[{ id: 'reader', appearance: serifItalicVoice }]}
      sessionKey="lobby/planning"
    />,
  );

  expect(screen.getByText('this').tagName).toBe('STRONG');
  expect(document.querySelector('script')).not.toBeInTheDocument();
  expect(screen.getByText('code').closest('pre')).toBeInTheDocument();
  expect(screen.getByText('Retired')).toBeInTheDocument();
  expect(screen.getByText('Still mine')).toBeInTheDocument();
  expect(screen.getByText('Streaming')).toBeInTheDocument();
  expect(screen.getByText('Stopped')).toBeInTheDocument();
  expect(screen.getByText('Failed')).toBeInTheDocument();
  expect(screen.getAllByRole('article')).toHaveLength(4);
  expect(document.querySelector('[data-status="complete"] .chaweb-entry-text'))
    .toHaveClass('cha-font-serif', 'cha-slant-italic');
  expect(screen.queryByText('complete')).not.toBeInTheDocument();
});

it('shows a multicast prompt once and keeps all four replies across snapshot updates', () => {
  const entries = Object.freeze(['one', 'two', 'three', 'four'].flatMap((character, index) => [
    entry({
      id: index * 2 + 1, kind: 'human', participant_id: 'reader', display_name: 'Reader',
      addressed_to: character, addressed_to_name: character,
      text: 'Shared question', request_id: index + 10, created_at: 100,
    }),
    entry({
      id: index * 2 + 2, participant_id: character, display_name: character,
      text: `${character} answer`, request_id: index + 10, created_at: 101 + index,
      status: index === 3 ? 'streaming' : 'complete',
    }),
  ]));
  const props = { characters: [], personas: [], layoutKey: 'compact:700', sessionKey: 'lobby/planning' };
  const { rerender } = render(<Transcript {...props} entries={entries} />);
  expect(screen.getAllByText('Shared question')).toHaveLength(1);
  expect(screen.getAllByText('Reader')).toHaveLength(1);
  for (const character of ['one', 'two', 'three', 'four']) {
    expect(screen.getByText(`${character} answer`)).toBeInTheDocument();
  }
  expect(screen.getAllByRole('article')).toHaveLength(5);
  expect(screen.getByText('Streaming')).toBeInTheDocument();
  expect(entries).toHaveLength(8);

  rerender(<Transcript {...props} entries={entries.map((item) => item.id === 8
    ? { ...item, text: 'four finished answer', status: 'complete' } : item)} />);
  expect(screen.getAllByText('Shared question')).toHaveLength(1);
  expect(screen.getByText('four finished answer')).toBeInTheDocument();
  expect(screen.queryByText('Streaming')).not.toBeInTheDocument();
  expect(screen.getAllByRole('article')).toHaveLength(5);
});

it('keeps changed prompts, different authors, and repeated character replies', () => {
  render(
    <Transcript
      characters={[]}
      entries={[
        entry({ id: 1, kind: 'human', participant_id: 'reader', text: 'First question' }),
        entry({ id: 2, text: 'Repeated answer' }),
        entry({ id: 3, kind: 'human', participant_id: 'reader', text: 'First question' }),
        entry({ id: 4, text: 'Repeated answer' }),
        entry({ id: 5, kind: 'human', participant_id: 'another', text: 'First question' }),
        entry({ id: 6, text: 'Another answer' }),
        entry({ id: 7, kind: 'human', participant_id: 'reader', text: 'First question' }),
        entry({ id: 8, kind: 'human', participant_id: 'reader', text: 'Second question' }),
        entry({ id: 9, kind: 'human', participant_id: 'reader', text: 'First question' }),
      ]}
      layoutKey="compact:700"
      personas={[]}
      sessionKey="lobby/planning"
    />,
  );
  expect(screen.getAllByText('First question')).toHaveLength(4);
  expect(screen.getByText('Second question')).toBeInTheDocument();
  expect(screen.getAllByText('Repeated answer')).toHaveLength(2);
  expect(screen.getByText('Another answer')).toBeInTheDocument();
  expect(screen.getAllByRole('article')).toHaveLength(8);
});

it('starts the prompt comparison again when switching sessions', () => {
  const entries = [entry({ kind: 'human', text: 'Same question' })];
  const props = { characters: [], personas: [], layoutKey: 'compact:700' };
  const { rerender } = render(<Transcript {...props} entries={entries} sessionKey="lobby/first" />);
  rerender(<Transcript {...props} entries={entries} sessionKey="lobby/second" />);
  expect(screen.getByText('Same question')).toBeInTheDocument();
});

it('follows the end until the reader scrolls away, including a layout change', () => {
  const first = [entry({ text: 'One' })];
  const { rerender } = render(
    <Transcript
      characters={[]}
      entries={first}
      layoutKey="compact:700"
      personas={[]}
      sessionKey="lobby/planning"
    />,
  );
  const scroller = screen.getByLabelText('Conversation transcript');
  const metrics = mockScroll(scroller, 1000, 100);
  rerender(
    <Transcript
      characters={[]}
      entries={[entry({ text: 'One' }), entry({ id: 2, text: 'Two' })]}
      layoutKey="compact:700"
      personas={[]}
      sessionKey="lobby/planning"
    />,
  );
  expect(metrics.scrollTop).toBe(1000);

  metrics.scrollTop = 0;
  fireEvent.scroll(scroller);
  rerender(
    <Transcript
      characters={[]}
      entries={[entry({ text: 'One' }), entry({ id: 2, text: 'Two' }), entry({ id: 3, text: 'Three' })]}
      layoutKey="expanded:700"
      personas={[]}
      sessionKey="lobby/planning"
    />,
  );
  expect(metrics.scrollTop).toBe(0);
});

it('keeps the editor mounted, focused, and unchanged when the transcript is replaced', () => {
  function Harness({ count }: { count: number }) {
    const [draft, setDraft] = useState('Dictated');
    const entries = Array.from({ length: count }, (_, index) => entry({
      id: index + 1,
      text: `Reply ${index + 1}`,
    }));
    return (
      <Conversation
        characters={[]}
        commandDisabled={false}
        deleteDisabled
        deleting={false}
        onDelete={() => {}}
        draft={draft}
        entries={entries}
        expanded={false}
        mode="send"
        notice={null}
        onDraft={setDraft}
        onExpanded={() => {}}
        onSend={() => {}}
        onSessions={() => {}}
        onStop={() => {}}
        personas={[]}
        sessionKey="lobby/planning"
        viewportHeight={700}
      />
    );
  }

  const { rerender } = render(<Harness count={1} />);
  const box = screen.getByRole('textbox', { name: 'Message' }) as HTMLTextAreaElement;
  box.focus();
  box.setSelectionRange(3, 3);
  rerender(<Harness count={2} />);
  expect(screen.getByRole('textbox', { name: 'Message' })).toBe(box);
  expect(box).toHaveFocus();
  expect(box).toHaveValue('Dictated');
  expect(box.selectionStart).toBe(3);
  expect(screen.getByText('Reply 2')).toBeInTheDocument();
});
