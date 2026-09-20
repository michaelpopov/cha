import { useCallback, useEffect, useState } from 'react';

import { publicErrorMessage, type ChaClient } from './api/client';

// Pass a stable loader (defined outside the component or memoized) so ordinary
// renders do not restart the request. Null data means loading or failed.
export function useLoad<Value>(
  client: ChaClient,
  load: (client: ChaClient) => Promise<Value>,
  failureMessage: string,
) {
  const [data, setData] = useState<Value | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [revision, setRevision] = useState(0);
  const retry = useCallback(() => setRevision((value) => value + 1), []);

  useEffect(() => {
    let current = true;
    setData(null);
    setError(null);
    async function run() {
      try {
        const loaded = await load(client);
        if (current) setData(loaded);
      } catch (failure: unknown) {
        if (current) setError(publicErrorMessage(failure, failureMessage));
      }
    }
    void run();
    return () => { current = false; };
  }, [client, load, failureMessage, revision]);

  return { data, error, retry };
}
