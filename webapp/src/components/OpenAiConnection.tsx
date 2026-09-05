import {
  useEffect,
  useRef,
  useState,
  type ReactNode,
} from 'react';

import {
  publicErrorMessage,
  type ChaClient,
  type OpenAiAuth,
} from '../api/client';
import type { AppState } from '../state/view';

type AuthMutation = 'login' | 'poll' | 'disconnect';

interface OpenAiConnectionScreenProps {
  state: AppState;
  client: ChaClient;
  sessionReport: ReactNode;
}

function verificationHref(snapshot: OpenAiAuth): string | null {
  if (snapshot.status !== 'waiting' || typeof snapshot.verification_url !== 'string') {
    return null;
  }
  return snapshot.verification_url.startsWith('https://') ? snapshot.verification_url : null;
}

export function OpenAiConnectionScreen({
  client,
  sessionReport,
}: OpenAiConnectionScreenProps) {
  const [snapshot, setSnapshot] = useState<OpenAiAuth | null>(null);
  const [httpError, setHttpError] = useState<string | null>(null);
  const [busy, setBusy] = useState<AuthMutation | null>(null);
  const [reloadToken, setReloadToken] = useState(0);
  const epoch = useRef(0);
  const cancelled = useRef(false);
  const timer = useRef<number | null>(null);
  const poll = useRef<(mine: number) => void>(() => undefined);

  function clearTimer() {
    if (timer.current === null) return;
    window.clearTimeout(timer.current);
    timer.current = null;
  }

  function accept(loaded: OpenAiAuth, mine: number) {
    if (epoch.current !== mine) return;
    setSnapshot(loaded);
    setHttpError(null);
    if (loaded.status !== 'waiting' || cancelled.current) {
      clearTimer();
      return;
    }
    clearTimer();
    const delay = typeof loaded.next_poll_delay_ms === 'number' && loaded.next_poll_delay_ms >= 0
      ? loaded.next_poll_delay_ms
      : 1000;
    timer.current = window.setTimeout(() => {
      timer.current = null;
      poll.current(mine);
    }, delay);
  }

  poll.current = (mine: number) => {
    if (epoch.current !== mine || cancelled.current) return;
    setBusy('poll');
    void client.pollOpenAiAuth().then(
      (loaded) => {
        if (epoch.current !== mine || cancelled.current) return;
        setBusy(null);
        accept(loaded, mine);
      },
      (failure: unknown) => {
        if (epoch.current !== mine || cancelled.current) return;
        setBusy(null);
        clearTimer();
        setHttpError(publicErrorMessage(
          failure,
          'ChatGPT connection status could not be updated.',
        ));
      },
    );
  };

  useEffect(() => {
    const mine = epoch.current + 1;
    epoch.current = mine;
    cancelled.current = false;
    setSnapshot(null);
    setHttpError(null);
    setBusy(null);
    clearTimer();
    void client.getOpenAiAuth().then(
      (loaded) => {
        if (epoch.current !== mine || cancelled.current) return;
        accept(loaded, mine);
      },
      (failure: unknown) => {
        if (epoch.current !== mine || cancelled.current) return;
        setHttpError(publicErrorMessage(
          failure,
          'ChatGPT connection status could not be loaded.',
        ));
      },
    );
    return () => {
      if (epoch.current === mine) epoch.current += 1;
      clearTimer();
    };
  }, [client, reloadToken]);

  async function connect() {
    if (busy) return;
    cancelled.current = false;
    const mine = epoch.current;
    setBusy('login');
    setHttpError(null);
    clearTimer();
    try {
      const loaded = await client.startOpenAiAuth();
      if (epoch.current !== mine || cancelled.current) return;
      setBusy(null);
      accept(loaded, mine);
    } catch (failure: unknown) {
      if (epoch.current !== mine || cancelled.current) return;
      setBusy(null);
      setHttpError(publicErrorMessage(failure, 'ChatGPT login could not be started.'));
    }
  }

  async function disconnect() {
    if (busy === 'login' || busy === 'disconnect') return;
    cancelled.current = true;
    clearTimer();
    const mine = epoch.current + 1;
    epoch.current = mine;
    setBusy('disconnect');
    setHttpError(null);
    try {
      const loaded = await client.disconnectOpenAiAuth();
      if (epoch.current !== mine) return;
      setBusy(null);
      accept(loaded, mine);
    } catch (failure: unknown) {
      if (epoch.current !== mine) return;
      setBusy(null);
      setHttpError(publicErrorMessage(
        failure,
        'ChatGPT could not be disconnected.',
      ));
    }
  }

  const message = httpError ?? snapshot?.error ?? null;
  const waiting = snapshot?.status === 'waiting';
  const href = snapshot ? verificationHref(snapshot) : null;

  return (
    <section className="cha-screen cha-navigation cha-settings" aria-label="Settings">
      {sessionReport}
      <section className="cha-settings-card" aria-labelledby="cha-openai-settings-title">
        <header className="cha-settings-card-header">
          <h2 id="cha-openai-settings-title">OpenAI</h2>
          <p>Connect ChatGPT to use your subscription with CHA.</p>
        </header>
        {snapshot === null && !httpError && (
          <div className="cha-settings-connection" role="status">
            <span className="cha-settings-status">
              <span aria-hidden="true" className="cha-settings-status-marker" />
              Loading ChatGPT connection…
            </span>
          </div>
        )}
        {message && (
          <div className="cha-settings-feedback cha-error-message" role="alert">
            <p>{message}</p>
            {httpError && (
              <button
                className="cha-button"
                onClick={() => setReloadToken((token) => token + 1)}
                type="button"
              >
                Try again
              </button>
            )}
          </div>
        )}
        {snapshot?.status === 'signed_out' && (
          <div className="cha-settings-connection">
            <span className="cha-settings-status">
              <span aria-hidden="true" className="cha-settings-status-marker" />
              Not connected
            </span>
            <button
              className="cha-button cha-button-primary"
              disabled={busy !== null}
              onClick={() => void connect()}
              type="button"
            >
              Connect ChatGPT
            </button>
          </div>
        )}
        {waiting && (
          <div className="cha-openai-verification">
            <div className="cha-settings-connection">
              <span className="cha-settings-status is-waiting">
                <span aria-hidden="true" className="cha-settings-status-marker" />
                Waiting for approval
              </span>
              <button
                className="cha-button"
                disabled={busy === 'login' || busy === 'disconnect'}
                onClick={() => void disconnect()}
                type="button"
              >
                Cancel
              </button>
            </div>
            {href && (
              <p>
                Open{' '}
                <a href={href} rel="noopener noreferrer" target="_blank">
                  the ChatGPT device page
                </a>
                {' '}and enter this code:
              </p>
            )}
            {snapshot.user_code && (
              <p className="cha-openai-code">{snapshot.user_code}</p>
            )}
          </div>
        )}
        {snapshot?.status === 'connected' && (
          <div className="cha-settings-connection">
            <span className="cha-settings-status is-connected">
              <span aria-hidden="true" className="cha-settings-status-marker" />
              Connected to ChatGPT
            </span>
            <button
              className="cha-button"
              disabled={busy !== null}
              onClick={() => void disconnect()}
              type="button"
            >
              Disconnect
            </button>
          </div>
        )}
      </section>
    </section>
  );
}
