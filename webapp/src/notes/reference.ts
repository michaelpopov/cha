const base62 = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';

export function noteReference(id: number): string | null {
  if (!Number.isSafeInteger(id) || id < 1 || id >= 62 ** 6) return null;

  let encoded = '';
  while (id > 0) {
    encoded = base62[id % 62] + encoded;
    id = Math.floor(id / 62);
  }
  return `##${encoded.padStart(6, '0')}`;
}
