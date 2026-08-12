import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';

import { ChaError } from '../api/client';
import { appReducer, initialAppState } from '../state/view';
import { bootstrapFixture } from '../test/fixtures';
import { Sidebar } from './Sidebar';

function readyState() {
  return appReducer(initialAppState, {
    type: 'bootstrap-loaded',
    bootstrap: bootstrapFixture,
  });
}

describe('Sidebar session actions', () => {
  it('offers the same rename/delete menu by right-click and ellipsis but not for Welcome', async () => {
    const user = userEvent.setup();
    render(
      <Sidebar
        dispatch={vi.fn()}
        onDeleteSession={vi.fn(async () => undefined)}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={vi.fn(async () => undefined)}
        state={readyState()}
      />,
    );

    expect(screen.queryByLabelText('Actions for Welcome')).not.toBeInTheDocument();
    await user.pointer({ keys: '[MouseRight]', target: screen.getByText('Planning') });
    expect(screen.getByRole('menuitem', { name: 'Rename…' })).toBeInTheDocument();
    expect(screen.getByRole('menuitem', { name: 'Delete…' })).toBeInTheDocument();
    await user.keyboard('{Escape}');

    await user.click(screen.getByLabelText('Actions for Planning'));
    await user.click(screen.getByRole('menuitem', { name: 'Rename…' }));
    expect(screen.getByRole('heading', { name: 'Rename session' })).toBeInTheDocument();
    expect(screen.getByLabelText('Session name')).toHaveValue('Planning');
  });

  it('confirms deletion before invoking it', async () => {
    const user = userEvent.setup();
    const onDelete = vi.fn(async () => undefined);
    render(
      <Sidebar
        dispatch={vi.fn()}
        onDeleteSession={onDelete}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={vi.fn(async () => undefined)}
        state={readyState()}
      />,
    );

    await user.click(screen.getByLabelText('Actions for Planning'));
    await user.click(screen.getByRole('menuitem', { name: 'Delete…' }));
    expect(screen.getByText(/removed from CHA and cannot be reopened/)).toBeInTheDocument();
    expect(onDelete).not.toHaveBeenCalled();
    await user.click(screen.getByRole('button', { name: 'Delete' }));
    expect(onDelete).toHaveBeenCalledWith('lobby', 'planning');
  });

  it('tracks expansion, supports menu keys, and restores focus on Escape', async () => {
    const user = userEvent.setup();
    render(
      <Sidebar
        dispatch={vi.fn()}
        onDeleteSession={vi.fn(async () => undefined)}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={vi.fn(async () => undefined)}
        state={readyState()}
      />,
    );

    const actions = screen.getByLabelText('Actions for Planning');
    expect(actions).toHaveAttribute('aria-expanded', 'false');
    await user.click(actions);
    expect(actions).toHaveAttribute('aria-expanded', 'true');
    const rename = screen.getByRole('menuitem', { name: 'Rename…' });
    const remove = screen.getByRole('menuitem', { name: 'Delete…' });
    expect(rename).toHaveFocus();
    await user.keyboard('{ArrowDown}');
    expect(remove).toHaveFocus();
    await user.keyboard('{Home}');
    expect(rename).toHaveFocus();
    await user.keyboard('{End}');
    expect(remove).toHaveFocus();
    await user.keyboard('{ArrowDown}');
    expect(rename).toHaveFocus();
    await user.keyboard('{ArrowUp}');
    expect(remove).toHaveFocus();
    await user.keyboard('{Escape}');
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(actions).toHaveAttribute('aria-expanded', 'false');
    expect(actions).toHaveFocus();
  });

  it('dismisses the menu when the reader points elsewhere', async () => {
    const user = userEvent.setup();
    render(
      <Sidebar
        dispatch={vi.fn()}
        onDeleteSession={vi.fn(async () => undefined)}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={vi.fn(async () => undefined)}
        state={readyState()}
      />,
    );

    await user.click(screen.getByLabelText('Actions for Planning'));
    expect(screen.getByRole('menu')).toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Forums' }));
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
  });

  it('restores focus to the Actions button when a dialog is cancelled', async () => {
    const user = userEvent.setup();
    render(
      <Sidebar
        dispatch={vi.fn()}
        onDeleteSession={vi.fn(async () => undefined)}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={vi.fn(async () => undefined)}
        state={readyState()}
      />,
    );

    const actions = screen.getByLabelText('Actions for Planning');
    await user.click(actions);
    await user.click(screen.getByRole('menuitem', { name: 'Rename…' }));
    expect(screen.getByLabelText('Session name')).toHaveFocus();
    await user.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(actions).toHaveFocus();
  });

  it('keeps an edited name and displays the public rename error', async () => {
    const user = userEvent.setup();
    const onRename = vi.fn(async () => {
      throw new ChaError(400, 'bad_request', 'Invalid session label.');
    });
    render(
      <Sidebar
        dispatch={vi.fn()}
        onDeleteSession={vi.fn(async () => undefined)}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={onRename}
        state={readyState()}
      />,
    );

    await user.click(screen.getByLabelText('Actions for Planning'));
    await user.click(screen.getByRole('menuitem', { name: 'Rename…' }));
    const input = screen.getByLabelText('Session name');
    await user.clear(input);
    await user.type(input, 'Revised name');
    await user.click(screen.getByRole('button', { name: 'Rename' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('Invalid session label.');
    expect(input).toHaveValue('Revised name');
    expect(onRename).toHaveBeenCalledWith('lobby', 'planning', 'Revised name');
    expect(screen.getByRole('dialog')).toBeInTheDocument();
  });

  it('disables the destructive action while deletion is pending', async () => {
    const user = userEvent.setup();
    let finish!: () => void;
    const pending = new Promise<void>((resolve) => { finish = resolve; });
    render(
      <Sidebar
        dispatch={vi.fn()}
        onDeleteSession={() => pending}
        onOpenSession={vi.fn(async () => true)}
        onRenameSession={vi.fn(async () => undefined)}
        state={readyState()}
      />,
    );

    await user.click(screen.getByLabelText('Actions for Planning'));
    await user.click(screen.getByRole('menuitem', { name: 'Delete…' }));
    await user.click(screen.getByRole('button', { name: 'Delete' }));
    expect(screen.getByRole('button', { name: 'Working…' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Cancel' })).toBeDisabled();
    finish();
  });
});
