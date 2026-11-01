import { describe, expect, it } from 'vitest';
import { newNoteImportSource } from './importSource';

describe('new note source', () => {
  it.each([
    ['/plan.md', { kind: 'file', path: '/plan.md' }],
    ['/todo', { kind: 'file', path: '/todo' }],
    ['  "/tmp/my plan.md"  ', { kind: 'file', path: '/tmp/my plan.md' }],
    ['C:\\Users\\me\\plan.md', { kind: 'file', path: 'C:\\Users\\me\\plan.md' }],
    ['"C:\\Users\\me\\my plan.md"', { kind: 'file', path: 'C:\\Users\\me\\my plan.md' }],
    ['C:/Users/me/plan.md', { kind: 'file', path: 'C:/Users/me/plan.md' }],
    ['\\\\server\\share\\plan.md', { kind: 'file', path: '\\\\server\\share\\plan.md' }],
    ['https://example.org/a', { kind: 'page', url: 'https://example.org/a' }],
    ['www.example.org/a', { kind: 'page', url: 'http://www.example.org/a' }],
    ['"https://example.org/a"', undefined],
    ['plan.md', undefined],
    ['./plan.md', undefined],
    ['../plan.md', undefined],
    ['~/plan.md', undefined],
    ['file:///tmp/plan.md', undefined],
    ['Read /tmp/plan.md', undefined],
    ['/tmp/a\ntext', undefined],
    ['/tmp/a\rtext', undefined],
    ['/tmp/a\0text', undefined],
  ])('classifies %s', (input, expected) => expect(newNoteImportSource(input)).toEqual(expected));
});
