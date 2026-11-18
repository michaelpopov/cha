import { describe, expect, it, vi } from 'vitest';

import type { NotesClient, SummaryTarget } from '../api/client';
import { fixtureClient } from '../test/fixtures';
import { noteDocument, noteItem, paragraph, textNode } from '../test/notes';
import { noteDocumentByteLimit } from './documentBytes';
import { runSummaryBatch } from './summaries';

type Deferred<T> = { promise: Promise<T>; resolve: (value: T) => void };

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>(done => { resolve = done; });
  return { promise, resolve };
}

const folder = noteItem({ id: 9, kind: 'folder' });
const alpha = { id: 1, name: 'Alpha * #', has_compressed: false, has_l1: false, has_l2: false };
const beta = { id: 2, name: 'Beta', has_compressed: true, has_l1: true, has_l2: true };
const heading = (name: string) => ({ type: 'heading', attrs: { level: 1 }, content: [textNode(name)] });
// Notes with every version, so a folder run makes only their sections.
const complete = (ids: number[]) => ids.map(id => ({
  id, name: `Note ${id}`, has_compressed: true, has_l1: true, has_l2: true,
}));

function setup(targets: SummaryTarget[] = [beta, alpha]) {
  const calls = {
    summaryLine: vi.fn<NotesClient['summaryLine']>(async () => {}),
    summaryCompress: vi.fn<NotesClient['summaryCompress']>(async () => true),
    summaryGenerate: vi.fn<NotesClient['summaryGenerate']>(async (id, part) =>
      part === 'l3_overview' ? 'Overview' : part === 'l3_section' ? `Section ${id}` : 'Summary'),
    summaryCommit: vi.fn<NotesClient['summaryCommit']>(async () => {}),
    summaryAbandon: vi.fn<NotesClient['summaryAbandon']>(async () => {}),
  };
  const client = fixtureClient(calls);
  const checkContext = vi.fn();
  const compressed = vi.fn(async (_id: number) => {});
  return {
    ...calls, checkContext, compressed,
    start: (source = folder) => runSummaryBatch(client, source, targets, {
      checkContext, commit: calls.summaryCommit, compressed,
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
    const compressed = deferred<boolean>();
    test.summaryCompress.mockReturnValueOnce(compressed.promise);
    const done = test.start();

    await vi.waitFor(() => expect(test.summaryGenerate).toHaveBeenCalledWith(2, 'l3_section'));
    expect(test.summaryCompress).toHaveBeenCalledExactlyOnceWith(1);
    // L1 waits for the Compress step of its note.
    expect(test.summaryGenerate.mock.calls.filter(([id]) => id === 1)).toEqual([]);
    compressed.resolve(true);
    await done;
    expect(test.summaryGenerate.mock.calls.filter(([id]) => id === 1)).toEqual([
      [1, 'l1'], [1, 'l2'], [1, 'l3_section'],
    ]);
    expect(test.summaryGenerate).toHaveBeenLastCalledWith(
      9, 'l3_overview', '# Alpha * #\n\nSection 1\n\n# Beta\n\nSection 2');
    expect(test.summaryLine.mock.calls).toEqual([[1, false], [2, false], [2, true], [1, true]]);
    expect(test.summaryLine.mock.invocationCallOrder.at(-1))
      .toBeLessThan(test.summaryGenerate.mock.invocationCallOrder.at(-1)!);
    expect(test.summaryCommit).toHaveBeenCalledWith(1, 'l2', noteDocument(heading(alpha.name), paragraph('Summary')));
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

  it('replaces both levels of a single note one after the other, with no Compress step', async () => {
    const test = setup([beta]);
    const level = deferred<string>();
    test.summaryGenerate.mockImplementationOnce(() => level.promise);
    const done = test.start(noteItem({ id: beta.id }));
    await vi.waitFor(() => expect(test.summaryGenerate).toHaveBeenCalledExactlyOnceWith(2, 'l1'));
    level.resolve('Level one');
    await done;
    expect(test.summaryGenerate.mock.calls).toEqual([[2, 'l1'], [2, 'l2']]);
    expect(test.summaryCommit.mock.calls.map(([id, part]) => [id, part])).toEqual([[2, 'l1'], [2, 'l2']]);
    expect(test.summaryCompress).not.toHaveBeenCalled();
  });

  it('continues a line after a failed step', async () => {
    const test = setup([alpha]);
    test.summaryCompress.mockRejectedValueOnce(new Error('Provider refused'));
    test.summaryGenerate.mockImplementation(async (_id, part) => {
      if (part === 'l1') throw new Error('Provider refused');
      return part === 'l3_overview' ? 'Overview' : 'Summary';
    });
    await test.start();
    expect(test.summaryGenerate.mock.calls.map(([id, part]) => [id, part])).toEqual([
      [1, 'l1'], [1, 'l2'], [1, 'l3_section'], [9, 'l3_overview'],
    ]);
    expect(test.summaryCommit.mock.calls.map(([id, part]) => [id, part])).toEqual([[1, 'l2'], [9, 'l3']]);
  });

  it('keeps independent level saves when L2 and the overview fail', async () => {
    const test = setup([alpha, beta]);
    test.summaryGenerate.mockImplementation(async (_id, part) => {
      if (part === 'l2' || part === 'l3_overview') throw new Error('Provider refused');
      return 'Valid';
    });
    await test.start();
    expect(test.summaryGenerate).toHaveBeenLastCalledWith(9, 'l3_overview', '# Alpha * #\n\nValid\n\n# Beta\n\nValid');
    expect(test.summaryCommit).toHaveBeenCalledExactlyOnceWith(1, 'l1', noteDocument(paragraph('Valid')));
  });

  it('starts no section after a failed section, keeps making levels, and sends no overview', async () => {
    const test = setup([...complete([1, 2, 3]), { id: 4, name: 'Note 4', has_compressed: true, has_l1: false, has_l2: true }]);
    const held = holdSections(test);
    test.summaryGenerate.mockImplementationOnce(async () => { throw new Error('Provider refused'); });
    const done = test.start();
    await vi.waitFor(() => expect([...held.keys()]).toEqual([2, 3]));
    for (const id of [2, 3]) held.get(id)!.resolve(`Section ${id}`);
    await done;
    expect(test.summaryGenerate.mock.calls).toEqual([[1, 'l3_section'], [2, 'l3_section'], [3, 'l3_section'], [4, 'l1']]);
    expect(test.summaryCommit.mock.calls.map(([id, part]) => [id, part])).toEqual([[4, 'l1']]);
  });

  it('lists only the notes that got a new Compressed version', async () => {
    const gamma = { id: 3, name: 'Gamma', has_compressed: false, has_l1: true, has_l2: true };
    const test = setup([alpha, gamma]);
    test.summaryCompress.mockImplementation(async id => id === 1);
    await test.start();
    expect(test.compressed).toHaveBeenCalledExactlyOnceWith(1);
    expect(test.summaryCompress.mock.calls).toEqual([[1], [3]]);
  });

  it('abandons the correct part after a conversion failure and continues other levels', async () => {
    const test = setup([alpha]);
    test.summaryGenerate.mockImplementation(async (_id, part) => part === 'l1' || part === 'l3_section' ? '' : 'Valid');
    await test.start();
    expect(test.summaryAbandon.mock.calls).toEqual([[1, 'l1'], [9, 'l3']]);
    expect(test.summaryCommit).toHaveBeenCalledExactlyOnceWith(1, 'l2', noteDocument(heading(alpha.name), paragraph('Valid')));
  });

  it('abandons L3 when individually valid documents exceed the combined size limit', async () => {
    const test = setup([beta]);
    const markdown = '```\n' + 'a'.repeat(noteDocumentByteLimit / 2) + '\n```';
    test.summaryGenerate.mockResolvedValue(markdown);
    await test.start();
    expect(test.summaryAbandon).toHaveBeenCalledExactlyOnceWith(9, 'l3');
    expect(test.summaryCommit).not.toHaveBeenCalled();
  });

  it.each(['Late level', ''])('stops the line after a vault change (%j)', async markdown => {
    const test = setup([alpha]);
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
    expect(test.summaryCommit).not.toHaveBeenCalled();
    expect(test.summaryAbandon).not.toHaveBeenCalled();
  });
});
