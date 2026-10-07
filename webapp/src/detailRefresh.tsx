import { createContext, useContext, useRef, useState, type ReactNode } from 'react';

// Welcome answers reload open forms through this value. Screens outside the
// desktop application see the idle default and keep their existing loads.
export interface DetailRefreshValue {
  epoch: number;
  refreshing: boolean;
  failed: boolean;
  retry(): void;
}

const idleRefresh: DetailRefreshValue = {
  epoch: 0,
  refreshing: false,
  failed: false,
  retry() {},
};

const DetailRefreshContext = createContext<DetailRefreshValue>(idleRefresh);

export function DetailRefreshProvider({
  value,
  children,
}: {
  value: DetailRefreshValue;
  children: ReactNode;
}) {
  return <DetailRefreshContext.Provider value={value}>{children}</DetailRefreshContext.Provider>;
}

export function useDetailRefresh(): DetailRefreshValue {
  return useContext(DetailRefreshContext);
}

export function sameDetail(left: unknown, right: unknown): boolean {
  return JSON.stringify(left) === JSON.stringify(right);
}

// Keeps a loaded form through a Welcome refresh. The first load replaces the
// form. A later load compares values: equal data keeps the draft, a clean form
// takes the new value, and a dirty form with new data becomes stale.
export function useFormReload(identity: string | null) {
  const refresh = useDetailRefresh();
  const seen = useRef<string | null | undefined>(undefined);
  const hasBaseline = useRef(false);
  const baseline = useRef<unknown>(undefined);
  const incoming = useRef<unknown>(undefined);
  const dirty = useRef(false);
  const [phase, setPhase] = useState<'idle' | 'loading' | 'failed' | 'stale'>('idle');
  const [attempt, setAttempt] = useState(0);

  function markDirty(value: boolean) {
    dirty.current = value;
  }

  function start(): boolean {
    if (seen.current !== identity) {
      seen.current = identity;
      hasBaseline.current = false;
      baseline.current = undefined;
      incoming.current = undefined;
    }
    const background = hasBaseline.current;
    setPhase(background ? 'loading' : 'idle');
    return background;
  }

  function loaded<T>(value: T, apply: (value: T) => void) {
    if (!hasBaseline.current || sameDetail(baseline.current, value)) {
      if (!hasBaseline.current) apply(value);
      baseline.current = value;
      hasBaseline.current = true;
      incoming.current = undefined;
      setPhase('idle');
      return;
    }
    if (dirty.current) {
      incoming.current = value;
      setPhase('stale');
      return;
    }
    baseline.current = value;
    incoming.current = undefined;
    setPhase('idle');
    apply(value);
  }

  function fail(background: boolean) {
    setPhase(background ? 'failed' : 'idle');
  }

  function accept<T>(apply: (value: T) => void) {
    if (incoming.current === undefined) return;
    const value = incoming.current as T;
    baseline.current = value;
    hasBaseline.current = true;
    incoming.current = undefined;
    setPhase('idle');
    apply(value);
  }

  function remember<T>(value: T) {
    baseline.current = value;
    hasBaseline.current = true;
    incoming.current = undefined;
    setPhase('idle');
  }

  return {
    epoch: refresh.epoch,
    attempt,
    start,
    loaded,
    fail,
    accept,
    remember,
    markDirty,
    retry: () => {
      if (refresh.failed) {
        refresh.retry();
        return;
      }
      setAttempt((value) => value + 1);
    },
    refreshing: phase === 'loading' || refresh.refreshing,
    blocked: phase !== 'idle' || refresh.refreshing || refresh.failed,
    refreshFailed: phase === 'failed' || refresh.failed,
    stale: phase === 'stale',
  };
}
