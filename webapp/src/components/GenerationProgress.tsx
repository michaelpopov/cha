import type { SessionSnapshot } from '../api/client';

type Generation = SessionSnapshot['generation'];

function sourceLabel(argument: string): { label: string; path: string } {
  try {
    const url = new URL(argument);
    return { label: url.host, path: url.pathname + url.search };
  } catch {
    return { label: argument, path: '' };
  }
}

export function GenerationProgress({ generation }: { generation: Generation }) {
  if (!generation.active) return null;
  const { progress, phase, character_display_name: name } = generation;
  const activities = phase === 'stopping' ? [] : progress?.activities ?? [];
  const reads = activities.filter((activity) => activity.tool === 'web_read').length;
  const searches = activities.length - reads;
  const subject = name || 'The character';
  let label = name ? `${name} is preparing a response…` : 'Preparing a response…';
  if (phase === 'stopping') label = `Stopping ${name}…`;
  else if (reads && searches) label = `${subject} is searching the web and reading ${reads === 1 ? 'a page' : `${reads} pages`}…`;
  else if (reads) label = `${subject} is reading ${reads === 1 ? 'a page' : `${reads} pages`}…`;
  else if (searches) label = `${subject} is searching the web…`;
  else if (phase === 'answering') label = `${subject} is writing the response…`;

  const counts: string[] = [];
  if (phase !== 'stopping' && progress) {
    if (progress.searches_completed) counts.push(`${progress.searches_completed} ${progress.searches_completed === 1 ? 'search' : 'searches'} completed`);
    if (progress.pages_read) counts.push(`${progress.pages_read} ${progress.pages_read === 1 ? 'page' : 'pages'} read`);
  }
  return (
    <div className="cha-generation">
      <div className="cha-generation-status cha-speaker">
        {phase !== 'stopping' && <span className="cha-generation-indicator" aria-hidden="true" />}
        <span aria-live="polite">{label}</span>
      </div>
      {activities.length > 0 ? (
        <details className="cha-generation-details">
          <summary>{counts.join(' · ') || (reads ? 'Reading pages' : 'Searching')}</summary>
          <div className="cha-generation-activities">
            {activities.map((activity) => {
              const source = activity.tool === 'web_read' ? sourceLabel(activity.argument) : null;
              return (
                <div className="cha-generation-activity" key={activity.id}>
                  <div>{source ? `Reading ${source.label}` : activity.argument}</div>
                  {source?.path && <div className="cha-generation-source">{source.path}</div>}
                  {activity.retrying && <div>Trying another reader…</div>}
                </div>
              );
            })}
          </div>
        </details>
      ) : counts.length > 0 && <div className="cha-generation-counts">{counts.join(' · ')}</div>}
    </div>
  );
}
