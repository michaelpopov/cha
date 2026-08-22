import { describe, expect, it, vi } from 'vitest';

import { ChaError } from '../api/client';
import { fixtureClient, snapshotFixture } from '../test/fixtures';
import {
  recoverSessionStream,
  reconnectingMessage,
  sessionProbe,
  waitingForCapacityMessage,
  type ProbeOutcome,
  type RecoverySteps,
} from './sessionRecovery';

// The ladder is driven entirely through these steps, so a test states what the
// session did on each rung and reads back what the ladder decided. No clock, no
// HTTP client, and no React are involved.
function drivableSteps(
  probes: ProbeOutcome[],
  attachResults: boolean[],
  overrides: Partial<RecoverySteps> = {},
) {
  const order: string[] = [];
  const reports: string[] = [];
  const waits: number[] = [];
  let probeIndex = 0;
  let attachIndex = 0;

  const steps: RecoverySteps = {
    probe: async () => {
      order.push('probe');
      return probes[probeIndex++] ?? 'unavailable';
    },
    report: (message) => {
      order.push('report');
      reports.push(message);
    },
    wait: async (milliseconds) => {
      order.push('wait');
      waits.push(milliseconds);
      return true;
    },
    attach: async () => {
      order.push('attach');
      return attachResults[attachIndex++] ?? false;
    },
    cancelled: () => false,
    ...overrides,
  };
  return { steps, order, reports, waits, attachCount: () => attachIndex };
}

describe('session probe', () => {
  function probeWith(overrides = {}, cancelled = () => false) {
    const snapshots: unknown[] = [];
    const probe = sessionProbe({
      client: fixtureClient(overrides),
      forumId: 'lobby',
      sessionId: 'planning',
      cancelled,
      onSnapshot: (snapshot) => snapshots.push(snapshot),
    });
    return { probe, snapshots };
  }

  function notLive() {
    return new ChaError(409, 'session_not_live', 'Session is not live.');
  }

  it('reports a session that answers as live and hands back its snapshot', async () => {
    const { probe, snapshots } = probeWith();

    expect(await probe()).toBe('live');
    expect(snapshots).toEqual([snapshotFixture]);
  });

  it('re-opens an unloaded session and reports it recovered', async () => {
    const openSession = vi.fn(async () => ({ forum_id: 'lobby', session_id: 'planning' }));
    const getSessionSnapshot = vi.fn()
      .mockRejectedValueOnce(notLive())
      .mockResolvedValueOnce(snapshotFixture);
    const { probe, snapshots } = probeWith({ openSession, getSessionSnapshot });

    expect(await probe()).toBe('recovered');
    expect(openSession).toHaveBeenCalledWith('lobby', 'planning');
    expect(snapshots).toEqual([snapshotFixture]);
  });

  it('reports a session it cannot re-open for want of capacity', async () => {
    const { probe } = probeWith({
      getSessionSnapshot: async () => { throw notLive(); },
      openSession: async () => {
        throw new ChaError(503, 'session_limit_reached', 'Session limit reached.');
      },
    });

    expect(await probe()).toBe('waiting-for-capacity');
  });

  it('reports an unreachable server rather than re-opening blindly', async () => {
    const openSession = vi.fn();
    const { probe } = probeWith({
      openSession,
      getSessionSnapshot: async () => { throw new Error('Server unavailable'); },
    });

    expect(await probe()).toBe('unavailable');
    expect(openSession).not.toHaveBeenCalled();
  });

  it('reports an unloaded session it could not re-open at all', async () => {
    const { probe } = probeWith({
      getSessionSnapshot: async () => { throw notLive(); },
      openSession: async () => { throw new Error('Server unavailable'); },
    });

    expect(await probe()).toBe('unavailable');
  });

  // A snapshot that lands after the user has left must not be pushed into a
  // conversation they are no longer looking at.
  it('withholds a snapshot that arrives after the conversation was left', async () => {
    const { probe, snapshots } = probeWith({}, () => true);

    expect(await probe()).toBe('unavailable');
    expect(snapshots).toEqual([]);
  });
});

describe('session stream recovery ladder', () => {
  it('probes, reports, waits, and attaches in that order on one rung', async () => {
    const driver = drivableSteps(['live'], [true]);

    expect(await recoverSessionStream([250], driver.steps)).toBe('connected');
    expect(driver.order).toEqual(['probe', 'report', 'wait', 'attach']);
    expect(driver.reports).toEqual([reconnectingMessage]);
    expect(driver.waits).toEqual([250]);
  });

  it('keeps climbing when a rung attaches a stream that fails first', async () => {
    const driver = drivableSteps(['live', 'live'], [false, true]);

    expect(await recoverSessionStream([250, 500], driver.steps)).toBe('connected');
    expect(driver.waits).toEqual([250, 500]);
    expect(driver.attachCount()).toBe(2);
  });

  it('attaches after a session the server had unloaded is opened again', async () => {
    const driver = drivableSteps(['recovered'], [true]);

    expect(await recoverSessionStream([250], driver.steps)).toBe('connected');
    expect(driver.attachCount()).toBe(1);
  });

  it('does not attach to a session it could not reach', async () => {
    const driver = drivableSteps(['unavailable', 'live'], [true]);

    expect(await recoverSessionStream([250, 500], driver.steps)).toBe('connected');
    expect(driver.order).toEqual([
      'probe', 'report', 'wait',
      'probe', 'report', 'wait', 'attach',
    ]);
  });

  it('names a session waiting for capacity while it keeps retrying', async () => {
    const driver = drivableSteps(['waiting-for-capacity', 'live'], [true]);

    expect(await recoverSessionStream([250, 500], driver.steps)).toBe('connected');
    expect(driver.reports).toEqual([waitingForCapacityMessage, reconnectingMessage]);
  });

  // A live session whose stream will not attach is a broken connection like
  // any other now: a session taken over on another device says so on the
  // stream itself and never reaches this ladder.
  it('reports a plain retry when the ladder runs out', async () => {
    const driver = drivableSteps(
      ['live', 'live', 'live'],
      [false, false, false],
    );

    expect(await recoverSessionStream([250, 500, 1_000], driver.steps)).toBe('retry');
    expect(driver.attachCount()).toBe(3);
  });

  it('reports a plain retry when a probe failed before the ladder ran out', async () => {
    const driver = drivableSteps(
      ['live', 'unavailable', 'live'],
      [false, false],
    );

    expect(await recoverSessionStream([250, 500, 1_000], driver.steps)).toBe('retry');
  });

  it('stops without reporting when a cancelled wait ends the ladder', async () => {
    const attach = vi.fn(async () => true);
    const driver = drivableSteps(['live'], [], {
      wait: async () => false,
      attach,
    });

    expect(await recoverSessionStream([250, 500], driver.steps)).toBe('cancelled');
    expect(attach).not.toHaveBeenCalled();
  });

  it('stops before probing once the conversation has been left', async () => {
    const probe = vi.fn(async (): Promise<ProbeOutcome> => 'live');
    const driver = drivableSteps([], [], { cancelled: () => true, probe });

    expect(await recoverSessionStream([250], driver.steps)).toBe('cancelled');
    expect(probe).not.toHaveBeenCalled();
  });

  it('discards a probe that landed after the conversation was left', async () => {
    let probed = false;
    const driver = drivableSteps([], [], {
      probe: async () => {
        probed = true;
        return 'live';
      },
      cancelled: () => probed,
    });

    expect(await recoverSessionStream([250], driver.steps)).toBe('cancelled');
    expect(driver.reports).toEqual([]);
  });
});
