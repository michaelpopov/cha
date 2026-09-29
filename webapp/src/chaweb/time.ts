export function formatTimestamp(unixSeconds: number, now = Date.now()): string {
  const date = new Date(unixSeconds * 1000);
  const current = new Date(now);
  const time = date.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });
  const day = date.toLocaleDateString(undefined, {
    month: 'short',
    day: 'numeric',
    ...(date.getFullYear() === current.getFullYear() ? {} : { year: 'numeric' }),
  });
  return `${day}, ${time}`;
}
