const base62 = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';

export function noteReference(id: number): string {
  let encoded = '';
  while (id > 0) {
    encoded = base62[id % 62] + encoded;
    id = Math.floor(id / 62);
  }
  return `##${encoded.padStart(6, '0')}`;
}
