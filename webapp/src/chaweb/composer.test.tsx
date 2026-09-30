import { fireEvent, render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { useState } from 'react';
import { afterEach, expect, it, vi } from 'vitest';

import { Composer, editorPixelHeight } from './composer';
import './styles.css';

afterEach(() => {
  vi.unstubAllGlobals();
});

function Editor({
  coarse = false,
  mode = 'send' as 'send' | 'stop',
  commandDisabled = false,
  onSend = () => {},
  onStop = () => {},
  onSessions = () => {},
  viewportHeight = 700,
}) {
  const [value, setValue] = useState('Hi');
  const [expanded, setExpanded] = useState(false);
  if (coarse) {
    vi.stubGlobal('matchMedia', (query: string) => ({
      matches: query === '(pointer: coarse)',
      media: query,
      onchange: null,
      addListener() {},
      removeListener() {},
      addEventListener() {},
      removeEventListener() {},
      dispatchEvent() { return false; },
    }));
  }
  return (
    <Composer
      commandDisabled={commandDisabled}
      copied={false}
      copyDisabled
      onCopy={() => {}}
      deleteDisabled
      deleting={false}
      expanded={expanded}
      mode={mode}
      onChange={setValue}
      onDelete={() => {}}
      onExpanded={setExpanded}
      onSend={onSend}
      onSessions={onSessions}
      onStop={onStop}
      value={value}
      viewportHeight={viewportHeight}
    />
  );
}

it('uses about two lines, then about half of the space above the controls', () => {
  expect(editorPixelHeight(false, 700)).toBe(56);
  expect(editorPixelHeight(true, 700)).toBe(306);
  render(<Editor />);
  expect(screen.getByRole('textbox', { name: 'Message' })).toHaveStyle({ height: '56px' });
});

it('sends on desktop Enter, inserts a newline on Ctrl+Enter, and ignores composition', async () => {
  const user = userEvent.setup();
  const onSend = vi.fn();
  render(<Editor onSend={onSend} />);
  const box = screen.getByRole('textbox', { name: 'Message' }) as HTMLTextAreaElement;
  box.focus();
  box.setSelectionRange(2, 2);
  fireEvent.compositionStart(box);
  fireEvent.keyDown(box, { key: 'Enter', code: 'Enter' });
  expect(onSend).not.toHaveBeenCalled();
  expect(box).toHaveValue('Hi');
  fireEvent.compositionEnd(box);

  await user.type(box, '{Enter}');
  expect(onSend).toHaveBeenCalledTimes(1);
  expect(box).toHaveValue('Hi');

  await user.type(box, '{Control>}{Enter}{/Control}');
  expect(onSend).toHaveBeenCalledTimes(1);
  expect(box).toHaveValue('Hi\n');
});

it('inserts a newline on a coarse pointer and does not send', async () => {
  const user = userEvent.setup();
  const onSend = vi.fn();
  render(<Editor coarse onSend={onSend} />);
  screen.getByRole('textbox', { name: 'Message' }).focus();
  await user.keyboard('{Enter}');
  expect(onSend).not.toHaveBeenCalled();
});

it('keeps Stop off Enter and leaves a disabled command idle', async () => {
  const user = userEvent.setup();
  const onSend = vi.fn();
  const onStop = vi.fn();
  const { rerender } = render(<Editor mode="stop" onSend={onSend} onStop={onStop} />);
  await user.click(screen.getByRole('button', { name: 'Stop' }));
  expect(onStop).toHaveBeenCalledTimes(1);
  expect(onSend).not.toHaveBeenCalled();
  screen.getByRole('textbox').focus();
  await user.keyboard('{Enter}');
  expect(onSend).not.toHaveBeenCalled();

  rerender(<Editor commandDisabled onSend={onSend} />);
  expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
  screen.getByRole('textbox').focus();
  await user.keyboard('{Enter}');
  expect(onSend).not.toHaveBeenCalled();
});

it('toggles with Enter and Space and keeps the editor focused for a pointer press', async () => {
  const user = userEvent.setup();
  render(<Editor />);
  const box = screen.getByRole('textbox', { name: 'Message' }) as HTMLTextAreaElement;
  fireEvent.change(box, { target: { value: 'Hello' } });
  box.focus();
  box.setSelectionRange(2, 2);

  const expand = screen.getByRole('button', { name: 'Expand editor' });
  const down = new PointerEvent('pointerdown', {
    bubbles: true, cancelable: true, button: 0, pointerType: 'touch',
  });
  expand.dispatchEvent(down);
  expect(down.defaultPrevented).toBe(true);
  expect(box).toHaveFocus();
  fireEvent.pointerUp(expand, { button: 0, pointerType: 'touch' });
  fireEvent.click(expand);
  expect(screen.getByRole('button', { name: 'Shrink editor' })).toBeInTheDocument();
  expect(box).toHaveFocus();
  expect(box.selectionStart).toBe(2);
  expect(box).toHaveValue('Hello');
  expect(box).toHaveStyle({ height: '306px' });

  const shrink = screen.getByRole('button', { name: 'Shrink editor' });
  shrink.focus();
  await user.keyboard('{Enter}');
  expect(screen.getByRole('button', { name: 'Expand editor' })).toHaveClass('chaweb-icon-button');
  screen.getByRole('button', { name: 'Expand editor' }).focus();
  await user.keyboard(' ');
  expect(screen.getByRole('button', { name: 'Shrink editor' })).toBeInTheDocument();
  expect(box).toHaveValue('Hello');
});

it('dismisses the keyboard from Sessions and does not grow with the draft', async () => {
  const user = userEvent.setup();
  const onSessions = vi.fn();
  render(<Editor onSessions={onSessions} />);
  const box = screen.getByRole('textbox', { name: 'Message' });
  box.focus();
  await user.click(screen.getByRole('button', { name: 'Sessions' }));
  expect(onSessions).toHaveBeenCalledTimes(1);
  expect(box).not.toHaveFocus();
  fireEvent.change(box, { target: { value: `${'line\n'.repeat(40)}end` } });
  expect(box).toHaveStyle({ height: '56px' });
});
