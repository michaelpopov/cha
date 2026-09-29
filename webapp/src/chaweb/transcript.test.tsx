import { fireEvent, render, screen } from '@testing-library/react';
import { useState } from 'react';
import { expect, it } from 'vitest';

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
