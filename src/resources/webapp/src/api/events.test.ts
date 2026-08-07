import { describe, expect, it, vi } from 'vitest';

import { snapshotFixture } from '../test/fixtures';
import { openSessionEvents } from './events';

class FakeEventSource {
  readonly listeners = new Map<string, (event: MessageEvent<string>) => void>();
  readonly close = vi.fn();
  onerror: ((event: Event) => void) | null = null;

  addEventListener(type: string, listener: (event: MessageEvent<string>) => void) {
    this.listeners.set(type, listener);
  }

  emit(type: string, payload: unknown) {
    this.listeners.get(type)?.(new MessageEvent(type, { data: JSON.stringify(payload) }));
  }
}

describe('session events', () => {
  it('dispatches typed snapshot and append events and closes idempotently', () => {
    const source = new FakeEventSource();
    const onSnapshot = vi.fn();
    const onAppend = vi.fn();
    const onError = vi.fn();
    const connection = openSessionEvents(
      'forum one',
      'session/two',
      { onSnapshot, onAppend, onError },
      (url) => {
        expect(url).toBe('/s/forum%20one/session%2Ftwo/api/v1/events');
        return source;
      },
    );

    source.emit('snapshot', snapshotFixture);
    source.emit('append', { target: { kind: 'entry', entry_id: 4 }, text: 'hello', seq: 1 });
    expect(onSnapshot).toHaveBeenCalledWith(snapshotFixture);
    expect(onAppend).toHaveBeenCalledWith({
      target: { kind: 'entry', entry_id: 4 },
      text: 'hello',
      seq: 1,
    });
    expect(onError).not.toHaveBeenCalled();

    connection.close();
    connection.close();
    expect(source.close).toHaveBeenCalledTimes(1);
    source.emit('append', { target: { kind: 'entry', entry_id: 4 }, text: 'ignored', seq: 2 });
    expect(onAppend).toHaveBeenCalledTimes(1);
  });

  it('reports malformed data and source errors only as a stream failure', () => {
    const source = new FakeEventSource();
    const onError = vi.fn();
    openSessionEvents(
      'forum',
      'session',
      { onSnapshot: vi.fn(), onAppend: vi.fn(), onError },
      () => source,
    );

    source.emit('append', { status: 409, error: { code: 'browser_stream_in_use' } });
    source.onerror?.(new Event('error'));
    expect(onError).toHaveBeenCalledTimes(1);
    expect(onError).toHaveBeenCalledWith({ kind: 'stream_failure' });
    expect(onError.mock.calls[0][0]).not.toHaveProperty('status');
  });
});
