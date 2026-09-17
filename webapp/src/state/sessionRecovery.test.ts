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

  it('classifies probe outcomes, re-opening only unloaded sessions', async () => {
    const cases = [
      { name: 'live', snapshotError: null, openError: null, outcome: 'live', opens: 0 },
      { name: 'unloaded', snapshotError: notLive(), openError: null, outcome: 'recovered', opens: 1 },
      { name: 'capacity', snapshotError: notLive(), openError: new ChaError(503, 'session_limit_reached', 'Session limit reached.'), outcome: 'waiting-for-capacity', opens: 1 },
      { name: 'unreachable', snapshotError: new Error('Server unavailable'), openError: null, outcome: 'unavailable', opens: 0 },
      { name: 'failed re-open', snapshotError: notLive(), openError: new Error('Server unavailable'), outcome: 'unavailable', opens: 1 },
    ] as const;
    for (const item of cases) {
      const openSession = vi.fn(async () => {
        if (item.openError) throw item.openError;
        return { forum_id: 'lobby', session_id: 'planning' };
      });
      const getSessionSnapshot = vi.fn().mockResolvedValue(snapshotFixture);
      if (item.snapshotError) getSessionSnapshot.mockRejectedValueOnce(item.snapshotError);
      const { probe, snapshots } = probeWith({ openSession, getSessionSnapshot });

      expect(await probe(), item.name).toBe(item.outcome);
      expect(openSession, item.name).toHaveBeenCalledTimes(item.opens);
      if (item.opens) expect(openSession).toHaveBeenCalledWith('lobby', 'planning');
      expect(snapshots, item.name).toEqual(
        item.outcome === 'live' || item.outcome === 'recovered' ? [snapshotFixture] : [],
      );
    }
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
  it('recovers live, reopened, temporarily unreachable, and capacity-limited sessions in order', async () => {
    const rung = ['probe', 'report', 'wait', 'attach'];
    const cases: Array<{
      name: string; probes: ProbeOutcome[]; attachments: boolean[]; delays: number[];
      order: string[]; reports: string[]; attaches: number;
    }> = [
      { name: 'live', probes: ['live'], attachments: [true], delays: [250], order: rung, reports: [reconnectingMessage], attaches: 1 },
      { name: 'failed attachment', probes: ['live', 'live'], attachments: [false, true], delays: [250, 500], order: [...rung, ...rung], reports: [reconnectingMessage, reconnectingMessage], attaches: 2 },
      { name: 'reopened', probes: ['recovered'], attachments: [true], delays: [250], order: rung, reports: [reconnectingMessage], attaches: 1 },
      { name: 'unreachable', probes: ['unavailable', 'live'], attachments: [true], delays: [250, 500], order: ['probe', 'report', 'wait', ...rung], reports: [reconnectingMessage, reconnectingMessage], attaches: 1 },
      { name: 'capacity', probes: ['waiting-for-capacity', 'live'], attachments: [true], delays: [250, 500], order: ['probe', 'report', 'wait', ...rung], reports: [waitingForCapacityMessage, reconnectingMessage], attaches: 1 },
    ];
    for (const item of cases) {
      const driver = drivableSteps(item.probes, item.attachments);
      expect(await recoverSessionStream(item.delays, driver.steps), item.name).toBe('connected');
      expect(driver.order, item.name).toEqual(item.order);
      expect(driver.reports, item.name).toEqual(item.reports);
      expect(driver.waits, item.name).toEqual(item.delays);
      expect(driver.attachCount(), item.name).toBe(item.attaches);
    }
  });

  // A live session whose stream will not attach is a broken connection like
  // any other now: a session taken over on another device says so on the
  // stream itself and never reaches this ladder.
  it('reports a plain retry when the ladder is exhausted, with or without a failed probe', async () => {
    const cases: Array<{ probes: ProbeOutcome[]; attachments: boolean[]; attaches: number }> = [
      { probes: ['live', 'live', 'live'], attachments: [false, false, false], attaches: 3 },
      { probes: ['live', 'unavailable', 'live'], attachments: [false, false], attaches: 2 },
    ];
    for (const item of cases) {
      const driver = drivableSteps(item.probes, item.attachments);
      expect(await recoverSessionStream([250, 500, 1_000], driver.steps), item.probes.join(',')).toBe('retry');
      expect(driver.attachCount()).toBe(item.attaches);
    }
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
