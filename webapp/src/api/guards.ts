export function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

export function hasIdentity(value: unknown): value is Record<string, unknown> & {
  id: string;
  display_name: string;
  description?: string;
} {
  return isRecord(value)
    && typeof value.id === 'string'
    && value.id.length > 0
    && typeof value.display_name === 'string'
    && (value.description === undefined || typeof value.description === 'string');
}
