import { act, fireEvent, render, screen, within } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import type { ActivityEvent } from '../api/client';
import { fixtureClient } from '../test/fixtures';
import { EventsScreen } from './EventsScreen';

const items: ActivityEvent[] = ['error', 'warning', 'success', 'info'].map((severity, index) => ({
  id: 4 - index,
  recorded_at: 1793048400,
  severity: severity as ActivityEvent['severity'],
  message: index === 0 ? '<a href="https://example.com">plain text</a>\n**No markdown**' : `Event ${index}`,
}));

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>(done => { resolve = done; });
  return { promise, resolve };
}

describe('Events screen', () => {
  it('shows loading, then an empty history, with the normal back control', async () => {
    const response = deferred<ActivityEvent[]>();
    const navigate = vi.fn();
    render(<EventsScreen client={fixtureClient({ listEvents: () => response.promise })} navigate={navigate} />);
    expect(screen.getByRole('status')).toHaveTextContent('Loading events…');
    expect(screen.queryByText('No recent events')).not.toBeInTheDocument();
    await act(async () => response.resolve([]));
    expect(screen.getByText('No recent events')).toBeInTheDocument();
    expect(screen.queryByRole('status')).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    expect(navigate).toHaveBeenCalledWith({ type: 'show-settings' });
  });

  it('keeps failures distinct from empty history and retries only the read', async () => {
    const listEvents = vi.fn().mockRejectedValueOnce(new Error('private diagnostic')).mockResolvedValueOnce(items);
    render(<EventsScreen client={fixtureClient({ listEvents })} navigate={vi.fn()} />);
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not load events');
    expect(screen.queryByText('No recent events')).not.toBeInTheDocument();
    expect(screen.queryByText('private diagnostic')).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Try again' }));
    expect(await screen.findAllByRole('listitem')).toHaveLength(4);
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    expect(listEvents).toHaveBeenCalledTimes(2);
  });

  it('shows static plain text in server order with accessible severity and local timestamps', async () => {
    const { container } = render(<EventsScreen client={fixtureClient({ listEvents: async () => items })} navigate={vi.fn()} />);
    const rows = await screen.findAllByRole('listitem');
    const format = new Intl.DateTimeFormat(undefined, { dateStyle: 'medium', timeStyle: 'medium' });
    for (const [index, label] of ['Error', 'Warning', 'Success', 'Information'].entries()) {
      expect(within(rows[index]).getByRole('img', { name: label })).toBeInTheDocument();
      const date = new Date(items[index].recorded_at * 1000);
      const time = rows[index].querySelector('time')!;
      expect(time).toHaveAttribute('datetime', date.toISOString());
      expect(time).toHaveTextContent(format.format(date));
      expect(rows[index]).toHaveTextContent(index === 0 ? '**No markdown**' : `Event ${index}`);
      expect(rows[index]).not.toHaveAttribute('tabindex');
    }
    expect(container.querySelector('a, strong, [aria-live]')).toBeNull();
    expect(screen.getAllByRole('button')).toHaveLength(1);
    expect(screen.queryByRole('heading')).not.toBeInTheDocument();
  });

  it('ignores a response from a view that has closed', async () => {
    const old = deferred<ActivityEvent[]>();
    const listEvents = vi.fn().mockReturnValueOnce(old.promise).mockResolvedValueOnce([]);
    const client = fixtureClient({ listEvents });
    const { rerender } = render(<EventsScreen client={client} navigate={vi.fn()} />);
    rerender(<div />);
    rerender(<EventsScreen client={client} navigate={vi.fn()} />);
    await screen.findByText('No recent events');
    await act(async () => old.resolve(items));
    expect(screen.queryByRole('listitem')).not.toBeInTheDocument();
    expect(screen.getByText('No recent events')).toBeInTheDocument();
  });
});
