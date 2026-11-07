import { fireEvent, render, screen } from '@testing-library/react';
import { expect, it, vi } from 'vitest';

import { bootstrapFixture } from '../test/fixtures';
import { welcomeSessionId } from '../state/route';
import { SessionsView } from './sessions';

function setup() {
  const props = {
    forums: bootstrapFixture.forums, forumId: 'lobby', entranceForumId: 'entrance',
    sessions: [
      { id: 'one', label: 'One', live: false, updated_at: 2 },
      { id: 'two', label: 'Two', live: true, updated_at: 1 },
      { id: welcomeSessionId, label: 'Entrance', live: false, updated_at: 0 },
    ],
    currentSessionId: null, error: null,
    onForum: vi.fn(), onOpen: vi.fn(), onNewSession: vi.fn(), onDelete: vi.fn(),
    canDelete: () => true, isDeleting: () => false,
  };
  render(<SessionsView {...props} />);
  return props;
}

function pointer(target: Element, type: string, x: number, y: number) {
  const event = new MouseEvent(type, { bubbles: true, clientX: x, clientY: y, button: 0 });
  Object.defineProperties(event, { isPrimary: { value: true }, pointerId: { value: 1 } });
  fireEvent(target, event);
}

function swipe(name: string, dx: number, dy = 0) {
  const button = screen.getByRole('button', { name: new RegExp(`^${name}`) });
  const row = button.closest('li')!;
  Object.defineProperty(row, 'setPointerCapture', { configurable: true, value: vi.fn() });
  pointer(button, 'pointerdown', 200, 20);
  pointer(button, 'pointermove', 200 + dx, 20 + dy);
  pointer(button, 'pointerup', 200 + dx, 20 + dy);
  return { button, row };
}

it('reveals Delete on swipe, suppresses opening, and deletes on a single explicit tap', () => {
  const props = setup();
  const { button, row } = swipe('One', -80);
  fireEvent.click(button); // The click generated at the end of a pointer gesture.
  expect(row).toHaveClass('is-revealed');
  expect(props.onOpen).not.toHaveBeenCalled();
  expect(props.onDelete).not.toHaveBeenCalled();
  const remove = screen.getByRole('button', { name: 'Delete session “One”' });
  pointer(remove, 'pointerdown', 280, 20);
  fireEvent.click(remove);
  expect(props.onDelete).toHaveBeenCalledExactlyOnceWith('one');
  expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
});

it('closes with a right swipe or an outside tap and reveals only one row', () => {
  setup();
  const first = swipe('One', -80);
  fireEvent.click(first.button);
  const second = swipe('Two', -80);
  fireEvent.click(second.button);
  expect(first.row).not.toHaveClass('is-revealed');
  expect(second.row).toHaveClass('is-revealed');
  swipe('Two', 80);
  fireEvent.click(second.button);
  expect(second.row).not.toHaveClass('is-revealed');
  swipe('One', -80);
  pointer(screen.getByRole('button', { name: 'New Session' }), 'pointerdown', 200, 250);
  expect(first.row).not.toHaveClass('is-revealed');
});

it('ignores vertical scrolling and cancels an unfinished gesture', () => {
  const props = setup();
  const { button, row } = swipe('One', -20, 70);
  expect(row).not.toHaveClass('is-revealed');
  pointer(button, 'pointerdown', 200, 20);
  pointer(button, 'pointermove', 120, 20);
  pointer(button, 'pointercancel', 120, 20);
  expect(row).not.toHaveClass('is-revealed');
  expect((row.querySelector('.chaweb-session-slide') as HTMLElement).style.transform).toBe('');
  expect(props.onDelete).not.toHaveBeenCalled();
});

it('keeps ordinary taps available, deletes on a second click, and protects Entrance', () => {
  const props = setup();
  fireEvent.click(screen.getByRole('button', { name: /^One/ }));
  expect(props.onOpen).toHaveBeenCalledExactlyOnceWith('one');
  const remove = screen.getByRole('button', { name: 'Delete session “Two”' });
  fireEvent.click(remove);
  expect(remove.closest('li')).toHaveClass('is-revealed');
  expect(props.onDelete).not.toHaveBeenCalled();
  fireEvent.click(remove);
  expect(props.onDelete).toHaveBeenCalledExactlyOnceWith('two');
  expect(screen.queryByRole('button', { name: 'Delete session “Entrance”' })).not.toBeInTheDocument();
});
