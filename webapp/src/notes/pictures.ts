export const pictureByteLimit = 1_048_576;
export const pictureTooLargeMessage = 'Picture exceeds 1 MiB. Choose a smaller picture.';
export const pictureTypeMessage = 'Choose a PNG, JPEG, WebP, or GIF picture.';
export const noteTooLargeMessage = 'Note exceeds 12 MiB. Remove content or use a smaller picture.';

export type PictureMime = 'image/png' | 'image/jpeg' | 'image/webp' | 'image/gif';

const pngSignature = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
const jpegSignature = [0xff, 0xd8, 0xff];
const gif87 = [0x47, 0x49, 0x46, 0x38, 0x37, 0x61];
const gif89 = [0x47, 0x49, 0x46, 0x38, 0x39, 0x61];
const riffSignature = [0x52, 0x49, 0x46, 0x46];
const webpSignature = [0x57, 0x45, 0x42, 0x50];
const imageExtensions = /\.(png|jpe?g|webp|gif)$/i;
const imageTypes = new Set([
  'image/png',
  'image/jpeg',
  'image/jpg',
  'image/pjpeg',
  'image/webp',
  'image/gif',
]);

function startsWith(bytes: Uint8Array, signature: readonly number[], offset = 0): boolean {
  if (bytes.length < offset + signature.length) return false;
  return signature.every((value, index) => bytes[offset + index] === value);
}

// Magic bytes decide the format. A valid header with bad pixels is still a picture.
export function detectImageMime(bytes: Uint8Array): PictureMime | null {
  if (startsWith(bytes, pngSignature)) return 'image/png';
  if (startsWith(bytes, jpegSignature)) return 'image/jpeg';
  if (startsWith(bytes, gif87) || startsWith(bytes, gif89)) return 'image/gif';
  if (startsWith(bytes, riffSignature) && startsWith(bytes, webpSignature, 8)) return 'image/webp';
  return null;
}

function encodeBase64(bytes: Uint8Array): string {
  let binary = '';
  const chunk = 0x8000;
  for (let index = 0; index < bytes.length; index += chunk) {
    const part = bytes.subarray(index, index + chunk);
    binary += String.fromCharCode(...part);
  }
  return btoa(binary);
}

export function pictureFromBytes(bytes: Uint8Array): { src: string } | { error: string } {
  if (bytes.byteLength > pictureByteLimit) return { error: pictureTooLargeMessage };
  const mime = detectImageMime(bytes);
  if (!mime) return { error: pictureTypeMessage };
  return { src: `data:${mime};base64,${encodeBase64(bytes)}` };
}

// Synchronous data-URL check. Filename and MIME labels are not enough on their own.
export function embeddedImageError(src: unknown): string | null {
  if (typeof src !== 'string' || !src.startsWith('data:')) return pictureTypeMessage;
  const match = /^data:([^;,]*);base64,([\s\S]*)$/i.exec(src);
  if (!match) return pictureTypeMessage;
  let binary: string;
  try {
    binary = atob(match[2]);
  } catch {
    return pictureTypeMessage;
  }
  if (binary.length > pictureByteLimit) return pictureTooLargeMessage;
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) bytes[index] = binary.charCodeAt(index);
  const mime = detectImageMime(bytes);
  if (!mime || mime !== match[1].toLowerCase()) return pictureTypeMessage;
  return null;
}

export function clipboardImageFile(data: DataTransfer | null): File | null {
  if (!data?.files) return null;
  for (const file of data.files) {
    const type = file.type.toLowerCase();
    if (imageTypes.has(type) || imageExtensions.test(file.name)) {
      return file;
    }
  }
  return null;
}

export function readFileBytes(file: File): { bytes: Promise<Uint8Array>; release(): void } {
  const reader = new FileReader();
  let finished = false;
  const bytes = new Promise<Uint8Array>((resolve, reject) => {
    const finish = () => {
      finished = true;
      reader.onload = null;
      reader.onerror = null;
      reader.onabort = null;
    };
    reader.onload = () => {
      const result = reader.result;
      finish();
      if (!(result instanceof ArrayBuffer)) {
        reject(new Error('read'));
        return;
      }
      resolve(new Uint8Array(result));
    };
    reader.onerror = () => {
      const error = reader.error ?? new Error('read');
      finish();
      reject(error);
    };
    reader.onabort = () => {
      finish();
      reject(new DOMException('aborted', 'AbortError'));
    };
    reader.readAsArrayBuffer(file);
  });
  return {
    bytes,
    release() {
      if (!finished) reader.abort();
      reader.onload = null;
      reader.onerror = null;
      reader.onabort = null;
    },
  };
}

export type NotePictureReader = (url: string) => Promise<Uint8Array>;

export async function loadNotePicture(src: string, readPicture: NotePictureReader): Promise<string | null> {
  if (src.startsWith('data:')) return embeddedImageError(src) ? null : src;
  try {
    const url = new URL(src);
    if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password) return null;
    const picture = pictureFromBytes(await readPicture(url.href));
    return 'src' in picture ? picture.src : null;
  } catch {
    return null;
  }
}
