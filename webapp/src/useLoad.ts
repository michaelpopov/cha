import { useCallback, useEffect, useRef, useState } from 'react';

import { publicErrorMessage, type ChaClient } from './api/client';
import { useDetailRefresh } from './detailRefresh';

// Pass a stable loader (defined outside the component or memoized) so ordinary
// renders do not restart the request. Null data means loading or failed.
export function useLoad<Value>(
  client: ChaClient,
  load: (client: ChaClient) => Promise<Value>,
  failureMessage: string,
) {
  const refresh = useDetailRefresh();
  const [data, setData] = useState<Value | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [revision, setRevision] = useState(0);
  const hasData = useRef(false);
  const retry = useCallback(() => {
    // Retry replaces the visible value. An epoch refresh leaves this set.
    hasData.current = false;
    setData(null);
    setError(null);
    setRevision((value) => value + 1);
  }, []);

  useEffect(() => {
    let current = true;
    const background = hasData.current;
    if (!background) {
      setData(null);
      setError(null);
    }
    async function run() {
      try {
        const loaded = await load(client);
        if (!current) return;
        hasData.current = true;
        setData(loaded);
        setError(null);
      } catch (failure: unknown) {
        if (current) setError(publicErrorMessage(failure, failureMessage));
      }
    }
    void run();
    return () => { current = false; };
  }, [client, load, failureMessage, refresh.epoch, revision]);

  return { data, error, retry };
}
