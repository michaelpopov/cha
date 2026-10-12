import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

import {
  noteDocumentByteLength,
  noteDocumentByteLimit,
  paddedNoteDocument,
} from './documentBytes';

const fixture = JSON.parse(readFileSync(join(
  dirname(fileURLToPath(import.meta.url)),
  '../../../tests/fixtures/notes/document-byte-limits.json',
), 'utf8')) as {
  limit: number;
  samples: Record<string, unknown>;
  sample_bytes: Record<string, number>;
  padding: { document: unknown };
};

describe('note document bytes', () => {
  it('measures the shared samples', () => {
    expect(fixture.limit).toBe(noteDocumentByteLimit);
    for (const [name, document] of Object.entries(fixture.samples)) {
      expect(noteDocumentByteLength(document)).toBe(fixture.sample_bytes[name]);
    }
  });

  it('builds an exact limit document and one byte over it', () => {
    const empty = paddedNoteDocument(fixture.padding.document, '');
    const overhead = noteDocumentByteLength(empty);
    const exact = paddedNoteDocument(
      fixture.padding.document,
      'a'.repeat(fixture.limit - overhead),
    );
    const over = paddedNoteDocument(
      fixture.padding.document,
      'a'.repeat(fixture.limit - overhead + 1),
    );
    expect(noteDocumentByteLength(exact)).toBe(fixture.limit);
    expect(noteDocumentByteLength(over)).toBe(fixture.limit + 1);
  });
});
