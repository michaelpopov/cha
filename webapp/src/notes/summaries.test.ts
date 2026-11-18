import { describe, expect, it, vi } from 'vitest';

import type { NotesClient, SummaryTarget } from '../api/client';
import { fixtureClient } from '../test/fixtures';
import { noteDocument, noteItem, paragraph, textNode } from '../test/notes';
import { noteDocumentByteLimit } from './documentBytes';
import { noteCommentColor } from './editor';
import { runSummaryBatch } from './summaries';

type Deferred<T> = { promise: Promise<T>; resolve: (value: T) => void };

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>(done => { resolve = done; });
  return { promise, resolve };
}

const folder = noteItem({ id: 9, kind: 'folder' });
const alpha: SummaryTarget = { id: 1, name: 'Alpha * #', levels: ['compressed', 'l1', 'l2', 'l3'] };
const beta: SummaryTarget = { id: 2, name: 'Beta', levels: ['l3'] };
const heading = (name: string) => ({ type: 'heading', attrs: { level: 1 }, content: [textNode(name)] });
// Notes that only need an L3.
const complete = (ids: number[]): SummaryTarget[] => ids.map(id => ({
  id, name: `Note ${id}`, levels: ['l3'],
}));

function setup(targets: SummaryTarget[] = [beta, alpha]) {
  const saved = new Map<number, Record<string, unknown>>();
  const calls = {
    getNote: vi.fn<NotesClient['getNote']>(async id => ({
      id, document_id: saved.has(id) ? id + 100 : id, document: saved.get(id) ?? noteDocument(paragraph('Full text')),
    })),
    summaryLine: vi.fn<NotesClient['summaryLine']>(async () => {}),
    summaryGenerate: vi.fn<NotesClient['summaryGenerate']>(async (id, part) =>
      part === 'l3_overview' ? 'Overview' : part === 'l3_section' ? `Section ${id}` : 'Summary'),
    summaryCommit: vi.fn<NotesClient['summaryCommit']>(async (id, part, document) => {
      if (part === 'l3_section') saved.set(id, document as Record<string, unknown>);
    }),
    summaryAbandon: vi.fn<NotesClient['summaryAbandon']>(async () => {}),
  };
  const client = fixtureClient(calls);
  const checkContext = vi.fn();
  return {
    ...calls, checkContext, saved,
    start: (source = folder, buildFolderSummary = source.kind === 'folder') => runSummaryBatch(client, source, targets, buildFolderSummary, {
      checkContext, commit: calls.summaryCommit,
    }),
  };
}

// Holds each section request until the test resolves it.
function holdSections(test: ReturnType<typeof setup>) {
  const held = new Map<number, Deferred<string>>();
  test.summaryGenerate.mockImplementation(async (id, part) => {
    if (part !== 'l3_section') return part === 'l3_overview' ? 'Overview' : 'Summary';
    const next = deferred<string>();
    held.set(id, next);
    return next.promise;
  });
  return held;
}

describe('runSummaryBatch', () => {
  it('runs the steps of each note in order and gives the overview the sections in tree order', async () => {
    const test = setup();
    const compressed = deferred<string>();
    test.summaryGenerate.mockImplementation(async (id, part) => {
      if (part === 'compressed') return compressed.promise;
      return part === 'l3_overview' ? 'Overview' : part === 'l3_section' ? `Section ${id}` : 'Summary';
    });
    const done = test.start();

    await vi.waitFor(() => expect(test.summaryGenerate).toHaveBeenCalledWith(2, 'l3_section'));
    // L1 waits for the Compressed step of its note.
    expect(test.summaryGenerate.mock.calls.filter(([id]) => id === 1)).toEqual([[1, 'compressed']]);
    compressed.resolve('Compressed text');
    await done;
    expect(test.summaryGenerate.mock.calls.filter(([id]) => id === 1)).toEqual([
      [1, 'compressed'], [1, 'l1'], [1, 'l2'], [1, 'l3_section'],
    ]);
    expect(test.summaryGenerate).toHaveBeenLastCalledWith(
      9, 'l3_overview', '# Alpha * #\n\nSection 1\n\n# Beta\n\nSection 2');
    expect(test.summaryLine.mock.calls).toEqual([[1, false], [2, false], [2, true], [1, true]]);
    expect(test.summaryLine.mock.invocationCallOrder.at(-1))
      .toBeLessThan(test.summaryGenerate.mock.invocationCallOrder.at(-1)!);
    expect(test.summaryCommit).toHaveBeenCalledWith(1, 'compressed', 'Compressed text');
    expect(test.summaryCommit).toHaveBeenCalledWith(1, 'l2', noteDocument(heading(alpha.name), paragraph('Summary')));
    expect(test.summaryCommit).toHaveBeenCalledWith(1, 'l3_section', noteDocument(paragraph('Section 1')));
    expect(test.getNote.mock.calls).toEqual([[1, 'l3'], [2, 'l3']]);
    expect(test.summaryCommit).toHaveBeenLastCalledWith(9, 'l3', noteDocument(
      heading(folder.name), paragraph('Overview'),
      heading(alpha.name), paragraph('Section 1'), heading(beta.name), paragraph('Section 2'),
    ));
  });

  it('keeps at most three lines and starts the next note when any line becomes free', async () => {
    const test = setup(complete([1, 2, 3, 4, 5]));
    const held = holdSections(test);
    const done = test.start();

    await vi.waitFor(() => expect([...held.keys()]).toEqual([1, 2, 3]));
    held.get(2)!.resolve('Two');
    await vi.waitFor(() => expect([...held.keys()]).toEqual([1, 2, 3, 4]));
    held.get(1)!.resolve('One');
    await vi.waitFor(() => expect([...held.keys()]).toEqual([1, 2, 3, 4, 5]));
    for (const id of [5, 4, 3]) held.get(id)!.resolve(`Section ${id}`);
    await done;
    expect(test.summaryGenerate).toHaveBeenLastCalledWith(9, 'l3_overview',
      '# Note 1\n\nOne\n\n# Note 2\n\nTwo\n\n# Note 3\n\nSection 3\n\n# Note 4\n\nSection 4\n\n# Note 5\n\nSection 5');
  });

  it('runs the selected levels of a single note in order', async () => {
    const test = setup([{ ...beta, levels: ['l2', 'l1'] }]);
    const level = deferred<string>();
    test.summaryGenerate.mockImplementationOnce(() => level.promise);
    const done = test.start(noteItem({ id: beta.id }));
    await vi.waitFor(() => expect(test.summaryGenerate).toHaveBeenCalledExactlyOnceWith(2, 'l1'));
    level.resolve('Level one');
    await done;
    expect(test.summaryGenerate.mock.calls).toEqual([[2, 'l1'], [2, 'l2']]);
    expect(test.summaryCommit.mock.calls.map(([id, part]) => [id, part])).toEqual([[2, 'l1'], [2, 'l2']]);
  });

  it('continues a line after a failed step', async () => {
    const test = setup([alpha]);
    test.summaryGenerate.mockImplementation(async (_id, part) => {
      if (part === 'l1' || part === 'compressed') throw new Error('Provider refused');
      return part === 'l3_overview' ? 'Overview' : 'Summary';
    });
    await test.start();
    expect(test.summaryGenerate.mock.calls.map(([id, part]) => [id, part])).toEqual([
      [1, 'compressed'], [1, 'l1'], [1, 'l2'], [1, 'l3_section'], [9, 'l3_overview'],
    ]);
    expect(test.summaryCommit.mock.calls.map(([id, part]) => [id, part])).toEqual([[1, 'l2'], [1, 'l3_section'], [9, 'l3']]);
  });

  it('keeps independent level saves when L2 and the overview fail', async () => {
    const test = setup([{ ...alpha, levels: ['l1', 'l2', 'l3'] }, beta]);
    test.summaryGenerate.mockImplementation(async (_id, part) => {
      if (part === 'l2' || part === 'l3_overview') throw new Error('Provider refused');
      return 'Valid';
    });
    await test.start();
    expect(test.summaryGenerate).toHaveBeenLastCalledWith(9, 'l3_overview', '# Alpha * #\n\nValid\n\n# Beta\n\nValid');
    expect(test.summaryCommit).toHaveBeenCalledWith(1, 'l1', noteDocument(paragraph('Valid')));
    expect([...test.saved.keys()].sort()).toEqual([1, 2]);
    expect(test.summaryCommit.mock.calls.every(([, part]) => part !== 'l3')).toBe(true);
  });

  it('keeps making later L3s after a failed overwrite and abandons the folder once', async () => {
    const test = setup([...complete([1, 2, 3]), { id: 4, name: 'Note 4', levels: ['l1', 'l3'] }]);
    const held = holdSections(test);
    const previous = noteDocument(paragraph('Old L3'));
    test.saved.set(1, previous);
    test.summaryGenerate.mockImplementationOnce(async () => { throw new Error('Provider refused'); });
    const done = test.start();
    await vi.waitFor(() => expect([...held.keys()]).toEqual([2, 3, 4]));
    for (const id of [2, 3, 4]) held.get(id)!.resolve(`Section ${id}`);
    await done;
    expect(test.summaryGenerate.mock.calls).toEqual([[1, 'l3_section'], [2, 'l3_section'], [3, 'l3_section'], [4, 'l1'], [4, 'l3_section']]);
    expect(test.summaryAbandon).toHaveBeenCalledExactlyOnceWith(folder.id, 'l3', 1);
    expect(test.getNote).not.toHaveBeenCalled();
    expect(test.saved.get(1)).toEqual(previous);
    expect([...test.saved.keys()].sort()).toEqual([1, 2, 3, 4]);
  });

  it.each(['compressed', 'l1', 'l2', 'l3'] as const)('makes only the %s request and write when selected alone', async level => {
    const test = setup([{ ...alpha, levels: [level] }]);
    const part = level === 'l3' ? 'l3_section' : level;
    await test.start(noteItem({ id: alpha.id }));
    expect(test.summaryGenerate).toHaveBeenCalledExactlyOnceWith(alpha.id, part);
    expect(test.summaryCommit).toHaveBeenCalledExactlyOnceWith(alpha.id, part,
      part === 'compressed' ? 'Summary' : expect.any(Object));
    expect(test.getNote).not.toHaveBeenCalled();
  });

  it('abandons the correct part after a conversion failure and continues other levels', async () => {
    const test = setup([{ ...alpha, levels: ['l1', 'l2', 'l3'] }]);
    test.summaryGenerate.mockImplementation(async (_id, part) => part === 'l1' || part === 'l3_section' ? '' : 'Valid');
    await test.start();
    expect(test.summaryAbandon.mock.calls).toEqual([[1, 'l1'], [1, 'l3_section'], [9, 'l3', 1]]);
    expect(test.summaryCommit).toHaveBeenCalledExactlyOnceWith(1, 'l2', noteDocument(heading(alpha.name), paragraph('Valid')));
  });

  it('abandons L3 when individually valid documents exceed the combined size limit', async () => {
    const test = setup([{ ...beta, levels: [] }]);
    const markdown = '```\n' + 'a'.repeat(noteDocumentByteLimit / 2) + '\n```';
    test.saved.set(beta.id, noteDocument(paragraph('b'.repeat(noteDocumentByteLimit / 2))));
    test.summaryGenerate.mockResolvedValue(markdown);
    await test.start();
    expect(test.summaryAbandon).toHaveBeenCalledExactlyOnceWith(9, 'l3');
    expect(test.summaryCommit).not.toHaveBeenCalled();
  });

  it('uses the same saved rich L3 for the overview and for the copy even if the L3 changes', async () => {
    const test = setup([{ ...alpha, levels: [] }, { ...beta, levels: [] }]);
    const overview = deferred<string>();
    test.summaryGenerate.mockReturnValue(overview.promise);
    const blocks = [
      heading('My heading'),
      { type: 'paragraph', content: [{ ...textNode('Edited passage'), marks: [
        { type: 'highlight', attrs: { color: '#ffff00' } },
        { type: 'link', attrs: { href: 'https://example.com' } },
      ] }] },
      { type: 'paragraph', content: [{ ...textNode('A comment'), marks: [
        { type: 'highlight', attrs: { color: noteCommentColor } },
      ] }] },
      { type: 'bulletList', content: [{ type: 'listItem', content: [paragraph('An item')] }] },
    ];
    test.saved.set(alpha.id, noteDocument(...blocks));
    test.saved.set(beta.id, noteDocument(paragraph('Saved Beta')));
    const done = test.start();
    await vi.waitFor(() => expect(test.summaryGenerate).toHaveBeenCalledExactlyOnceWith(folder.id, 'l3_overview',
      expect.stringMatching(/^# Alpha \* #\n\nMy heading\n\nEdited passage\n\nA comment\n{2,}An item\n\n# Beta\n\nSaved Beta$/)));
    test.saved.set(alpha.id, noteDocument(paragraph('Later edit')));
    overview.resolve('Overview');
    await done;
    expect(test.getNote.mock.calls).toEqual([[alpha.id, 'l3'], [beta.id, 'l3']]);
    expect(test.summaryLine).not.toHaveBeenCalled();
    expect(test.summaryCommit).toHaveBeenCalledExactlyOnceWith(folder.id, 'l3', noteDocument(
      heading(folder.name), paragraph('Overview'), heading(alpha.name), ...blocks, heading(beta.name), paragraph('Saved Beta')));
  });

  it('generates only the missing L3 when two of three contributors already have one', async () => {
    const test = setup([{ ...alpha, levels: [] }, beta, { id: 3, name: 'Gamma', levels: [] }]);
    test.saved.set(alpha.id, noteDocument(paragraph('Saved Alpha')));
    test.saved.set(3, noteDocument(paragraph('Saved Gamma')));
    await test.start();
    expect(test.summaryGenerate.mock.calls).toEqual([
      [beta.id, 'l3_section'],
      [folder.id, 'l3_overview', '# Alpha * #\n\nSaved Alpha\n\n# Beta\n\nSection 2\n\n# Gamma\n\nSaved Gamma'],
    ]);
    expect(test.summaryLine.mock.calls).toEqual([[beta.id, false], [beta.id, true]]);
    expect(test.getNote.mock.calls).toEqual([[alpha.id, 'l3'], [beta.id, 'l3'], [3, 'l3']]);
  });

  it.each([false, true])('abandons a folder when its cached L3 is unavailable (read error %s)', async readError => {
    const test = setup([{ ...beta, levels: [] }]);
    if (readError) test.getNote.mockRejectedValueOnce(new Error('Could not read note'));
    await test.start();
    expect(test.summaryAbandon).toHaveBeenCalledExactlyOnceWith(folder.id, 'l3', beta.id);
    expect(test.summaryGenerate).not.toHaveBeenCalled();
    expect(test.summaryCommit).not.toHaveBeenCalled();
  });

  it('abandons the folder after a required section write fails and keeps other writes', async () => {
    const test = setup([beta, { ...alpha, levels: ['l3'] }]);
    test.summaryCommit.mockRejectedValueOnce(new Error('Write failed'));
    await test.start();
    expect(test.summaryAbandon).toHaveBeenCalledExactlyOnceWith(folder.id, 'l3', alpha.id);
    expect(test.saved.has(beta.id)).toBe(true);
    expect(test.getNote).not.toHaveBeenCalled();
  });

  it.each(['Late level', ''])('stops the line after a vault change (%j)', async markdown => {
    const test = setup([{ ...alpha, levels: ['l1', 'l2'] }]);
    const level = deferred<string>();
    test.summaryGenerate.mockImplementationOnce(() => level.promise);
    const changed = new Error('The vault changed');
    const done = test.start();
    await vi.waitFor(() => expect(test.summaryGenerate).toHaveBeenCalledExactlyOnceWith(1, 'l1'));
    test.checkContext.mockImplementation(() => { throw changed; });
    level.resolve(markdown);

    await expect(done).rejects.toBe(changed);
    expect(test.summaryGenerate).toHaveBeenCalledOnce();
    expect(test.summaryCommit).not.toHaveBeenCalled();
    expect(test.summaryAbandon).not.toHaveBeenCalled();
  });

  it('discards the overview reply after a vault change', async () => {
    const test = setup([beta]);
    const overview = deferred<string>();
    test.summaryGenerate.mockResolvedValueOnce('Section').mockReturnValueOnce(overview.promise);
    const done = test.start();
    await vi.waitFor(() => expect(test.summaryGenerate).toHaveBeenLastCalledWith(9, 'l3_overview', '# Beta\n\nSection'));
    const changed = new Error('The vault changed');
    overview.resolve('Overview');
    test.checkContext.mockImplementation(() => { throw changed; });

    await expect(done).rejects.toBe(changed);
    expect(test.summaryCommit).toHaveBeenCalledExactlyOnceWith(beta.id, 'l3_section', noteDocument(paragraph('Section')));
    expect(test.summaryAbandon).not.toHaveBeenCalled();
  });
});
