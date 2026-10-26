import { useEffect, useState } from 'react';
import type { ActivityEvent } from '../api/client';
import { CheckIcon, ErrorIcon, InformationIcon, WarningIcon } from './Icons';
import { BackToSettings, LoadFailure, type SettingsScreenProps } from './SettingsShared';

const severityLabels = { info: 'Information', success: 'Success', warning: 'Warning', error: 'Error' };

export function EventsScreen({ client, navigate }: Pick<SettingsScreenProps, 'client' | 'navigate'>) {
  const [events, setEvents] = useState<ActivityEvent[] | null>(null);
  const [failed, setFailed] = useState(false);
  const [attempt, setAttempt] = useState(0);

  useEffect(() => {
    let current = true;
    setEvents(null);
    setFailed(false);
    void client.listEvents().then(
      (items) => { if (current) setEvents(items); },
      () => { if (current) setFailed(true); },
    );
    return () => { current = false; };
  }, [client, attempt]);

  const format = new Intl.DateTimeFormat(undefined, { dateStyle: 'medium', timeStyle: 'medium' });

  return <section className="cha-screen cha-navigation" aria-label="Events">
    <BackToSettings navigate={navigate} />
    {failed ? <LoadFailure message="Could not load events" retry={() => setAttempt(value => value + 1)} />
      : events === null ? <p className="cha-state-message" role="status">Loading events…</p>
        : events.length === 0 ? <p className="cha-state-message">No recent events</p>
          : <ul className="cha-event-list">
            {events.map(event => {
              const date = new Date(event.recorded_at * 1000);
              return <li className="cha-event-row" key={event.id}>
                <div className="cha-event-meta">
                  <span role="img" aria-label={severityLabels[event.severity]}>
                    {event.severity === 'info' ? <InformationIcon />
                      : event.severity === 'success' ? <CheckIcon />
                        : event.severity === 'warning' ? <WarningIcon /> : <ErrorIcon />}
                  </span>
                  <time dateTime={date.toISOString()}>{format.format(date)}</time>
                </div>
                <p className="cha-event-message">{event.message}</p>
              </li>;
            })}
          </ul>}
  </section>;
}
