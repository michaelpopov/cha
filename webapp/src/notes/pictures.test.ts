import { describe, expect, it } from 'vitest';
import {
  clipboardImageFile,
  embeddedImageError,
  pictureByteLimit,
  pictureFromBytes,
  pictureTooLargeMessage,
  pictureTypeMessage,
  readFileBytes,
} from './pictures';

const pngHeader = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
const jpegHeader = [0xff, 0xd8, 0xff];
const gif87 = [...'GIF87a'].map((char) => char.charCodeAt(0));
const gif89 = [...'GIF89a'].map((char) => char.charCodeAt(0));
const webpHeader = [0x52, 0x49, 0x46, 0x46, 0, 0, 0, 0, 0x57, 0x45, 0x42, 0x50];

function bytes(length: number, header: readonly number[]): Uint8Array<ArrayBuffer> {
  const value = new Uint8Array(length);
  value.set(header);
  return value;
}

function dataUrl(mime: string, value: Uint8Array): string {
  let binary = '';
  for (let index = 0; index < value.length; index += 0x8000) {
    binary += String.fromCharCode(...value.subarray(index, index + 0x8000));
  }
  return `data:${mime};base64,${btoa(binary)}`;
}

describe('picture bytes', () => {
  it('encodes PNG, JPEG, GIF, and WebP from magic bytes', () => {
    const png = pictureFromBytes(bytes(12, pngHeader));
    const jpeg = pictureFromBytes(bytes(8, jpegHeader));
    const gif = pictureFromBytes(bytes(10, gif89));
    const olderGif = pictureFromBytes(bytes(10, gif87));
    const webp = pictureFromBytes(bytes(16, webpHeader));
    expect(png).toMatchObject({ src: expect.stringMatching(/^data:image\/png;base64,/) });
    expect(jpeg).toMatchObject({ src: expect.stringMatching(/^data:image\/jpeg;base64,/) });
    expect(gif).toMatchObject({ src: expect.stringMatching(/^data:image\/gif;base64,/) });
    expect(olderGif).toMatchObject({ src: expect.stringMatching(/^data:image\/gif;base64,/) });
    expect(webp).toMatchObject({ src: expect.stringMatching(/^data:image\/webp;base64,/) });
    if (!('src' in png) || !('src' in jpeg)) throw new Error('expected pictures');
    expect(embeddedImageError(png.src)).toBeNull();
    expect(embeddedImageError(jpeg.src)).toBeNull();
    expect(embeddedImageError(jpeg.src.replace('image/jpeg', 'image/jpg'))).toBe(pictureTypeMessage);
  });

  it('accepts a valid header with bad pixels and an exact 1 MiB file', () => {
    const broken = pictureFromBytes(bytes(10, [...pngHeader, 0xff, 0xff]));
    expect(broken).toMatchObject({ src: expect.stringMatching(/^data:image\/png;base64,/) });
    const exact = pictureFromBytes(bytes(pictureByteLimit, pngHeader));
    expect(exact).toMatchObject({ src: expect.stringMatching(/^data:image\/png;base64,/) });
    if (!('src' in exact)) throw new Error('expected an exact-size picture');
    expect(embeddedImageError(exact.src)).toBeNull();
  });

  it('rejects an over-limit file before the format check', () => {
    expect(pictureFromBytes(bytes(pictureByteLimit + 1, pngHeader))).toEqual({
      error: pictureTooLargeMessage,
    });
    expect(embeddedImageError(dataUrl('image/png', bytes(pictureByteLimit + 1, pngHeader)))).toBe(
      pictureTooLargeMessage,
    );
  });

  it('rejects bad Base64, a MIME mismatch, and remote references', () => {
    expect(embeddedImageError('data:image/png;base64,%%%')).toBe(pictureTypeMessage);
    const png = pictureFromBytes(bytes(8, pngHeader));
    if (!('src' in png)) throw new Error('expected a png');
    expect(embeddedImageError(png.src.replace('image/png', 'image/gif'))).toBe(pictureTypeMessage);
    expect(embeddedImageError('blob:https://example.com/id')).toBe(pictureTypeMessage);
    expect(embeddedImageError('file:///tmp/a.png')).toBe(pictureTypeMessage);
    expect(embeddedImageError('https://example.com/a.png')).toBe(pictureTypeMessage);
    expect(pictureFromBytes(Uint8Array.from([1, 2, 3, 4]))).toEqual({ error: pictureTypeMessage });
  });

  it('chooses a clipboard image file by MIME or extension', () => {
    const text = new File([Uint8Array.from([1])], 'notes.txt', { type: 'text/plain' });
    const png = new File([Uint8Array.from([1])], 'a.png', { type: 'image/png' });
    const named = new File([Uint8Array.from([1])], 'a.jpeg', { type: '' });
    const svg = new File([Uint8Array.from([1])], 'a.svg', { type: 'image/svg+xml' });
    const bmp = new File([Uint8Array.from([1])], 'a.bmp', { type: 'image/bmp' });
    const data = { files: [text, png] } as unknown as DataTransfer;
    expect(clipboardImageFile(data)).toBe(png);
    expect(clipboardImageFile({ files: [text] } as unknown as DataTransfer)).toBeNull();
    expect(clipboardImageFile({ files: [named] } as unknown as DataTransfer)).toBe(named);
    expect(clipboardImageFile({ files: [svg, png] } as unknown as DataTransfer)).toBe(png);
    expect(clipboardImageFile({ files: [bmp] } as unknown as DataTransfer)).toBeNull();
    expect(clipboardImageFile(null)).toBeNull();
  });

  it('reads a file and releases the reader', async () => {
    const payload = bytes(8, pngHeader);
    const read = readFileBytes(new File([payload], 'a.png', { type: 'image/png' }));
    await expect(read.bytes).resolves.toEqual(payload);
    read.release();

    const cancelled = readFileBytes(new File([payload], 'b.png', { type: 'image/png' }));
    cancelled.release();
    await expect(cancelled.bytes).rejects.toMatchObject({ name: 'AbortError' });
  });
});
