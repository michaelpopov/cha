import { describe, expect, it, vi } from 'vitest';

import type { NotesClient, SummaryTarget } from '../api/client';
import { fixtureClient } from '../test/fixtures';
import { noteDocument, noteItem, paragraph, textNode } from '../test/notes';
import { noteDocumentByteLimit } from './documentBytes';
import { summarizeAndWait } from './summaries';

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
    summaryPlan: vi.fn<NotesClient['summaryPlan']>(async () => ({ notes: targets })),
    summaryLine: vi.fn<NotesClient['summaryLine']>(async () => {}),
    summaryCompress: vi.fn<NotesClient['summaryCompress']>(async () => true),
    summaryGenerate: vi.fn<NotesClient['summaryGenerate']>(async (id, part) =>
      part === 'l3_overview' ? 'Overview' : part === 'l3_section' ? `Section ${id}` : 'Summary'),
    summaryCommit: vi.fn<NotesClient['summaryCommit']>(async () => {}),
    summaryAbandon: vi.fn<NotesClient['summaryAbandon']>(async () => {}),
    summaryCancel: vi.fn<NotesClient['summaryCancel']>(async () => {}),
  };
  const client = fixtureClient(calls);
  const run: Parameters<typeof summarizeAndWait>[2] = { requested: false };
  const checkContext = vi.fn();
  const progress = vi.fn();
  return {
    ...calls, run, checkContext, progress,
    start: (source = folder) => summarizeAndWait(client, source, run, checkContext, progress),
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

describe('summarizeAndWait', () => {
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
    expect(await done).toEqual({ targetIds: [1, 2], compressedIds: [1], folderSummaryCommitted: true });
    expect(test.summaryPlan).toHaveBeenCalledExactlyOnceWith(9);
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
    expect(test.progress.mock.calls.map(([value]) => value)).toEqual([
      { message: 'Summarizing “Alpha * #”…', count: 'Note 1 of 2', cancelEnabled: true },
      { message: 'Summarizing “Beta”…', count: 'Note 2 of 2', cancelEnabled: true },
      { message: 'Writing the Summary note…', cancelEnabled: true },
      { message: 'Writing the Summary note…', cancelEnabled: false },
    ]);
    expect(test.progress.mock.invocationCallOrder.at(-1)).toBeLessThan(test.summaryCommit.mock.invocationCallOrder.at(-1)!);
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
    expect(await done).toEqual({ targetIds: [1, 2, 3, 4, 5], compressedIds: [], folderSummaryCommitted: true });
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
    expect(await done).toEqual({ targetIds: [2], compressedIds: [], folderSummaryCommitted: false });
    expect(test.summaryGenerate.mock.calls).toEqual([[2, 'l1'], [2, 'l2']]);
    expect(test.summaryCommit.mock.calls.map(([id, part]) => [id, part])).toEqual([[2, 'l1'], [2, 'l2']]);
    expect(test.summaryCompress).not.toHaveBeenCalled();
    expect(test.progress.mock.calls.map(([value]) => value)).toEqual([
      { message: 'Summarizing “Beta”…', count: undefined, cancelEnabled: true },
    ]);
  });

  it('continues a line after a failed step', async () => {
    const test = setup([alpha]);
    test.summaryCompress.mockRejectedValueOnce(new Error('Provider refused'));
    test.summaryGenerate.mockImplementation(async (_id, part) => {
      if (part === 'l1') throw new Error('Provider refused');
      return part === 'l3_overview' ? 'Overview' : 'Summary';
    });
    expect(await test.start()).toEqual({ targetIds: [1], compressedIds: [], folderSummaryCommitted: true });
    expect(test.summaryGenerate.mock.calls.map(([id, part]) => [id, part])).toEqual([
      [1, 'l1'], [1, 'l2'], [1, 'l3_section'], [9, 'l3_overview'],
    ]);
    expect(test.summaryCommit.mock.calls.map(([id, part]) => [id, part])).toEqual([[1, 'l2'], [9, 'l3']]);
  });

  it('starts no section after a failed section, keeps making levels, and sends no overview', async () => {
    const test = setup([...complete([1, 2, 3]), { id: 4, name: 'Note 4', has_compressed: true, has_l1: false, has_l2: true }]);
    const held = holdSections(test);
    test.summaryGenerate.mockImplementationOnce(async () => { throw new Error('Provider refused'); });
    const done = test.start();
    await vi.waitFor(() => expect([...held.keys()]).toEqual([2, 3]));
    for (const id of [2, 3]) held.get(id)!.resolve(`Section ${id}`);
    expect(await done).toEqual({ targetIds: [1, 2, 3, 4], compressedIds: [], folderSummaryCommitted: false });
    expect(test.summaryGenerate.mock.calls).toEqual([[1, 'l3_section'], [2, 'l3_section'], [3, 'l3_section'], [4, 'l1']]);
    expect(test.summaryCommit.mock.calls.map(([id, part]) => [id, part])).toEqual([[4, 'l1']]);
  });

  it('lists only the notes that got a new Compressed version', async () => {
    const gamma = { id: 3, name: 'Gamma', has_compressed: false, has_l1: true, has_l2: true };
    const test = setup([alpha, gamma]);
    test.summaryCompress.mockImplementation(async id => id === 1);
    expect((await test.start()).compressedIds).toEqual([1]);
    expect(test.summaryCompress.mock.calls).toEqual([[1], [3]]);
  });

  it('makes no generation calls for an empty plan', async () => {
    const test = setup([]);
    expect(await test.start()).toEqual({ targetIds: [], compressedIds: [], folderSummaryCommitted: false });
    expect(test.summaryGenerate).not.toHaveBeenCalled();
    expect(test.summaryCommit).not.toHaveBeenCalled();
    expect(test.progress).not.toHaveBeenCalled();
  });

  it('ignores a late level after Cancel and starts no more steps', async () => {
    const test = setup([alpha]);
    const late = deferred<string>();
    test.summaryGenerate.mockImplementationOnce(() => late.promise);
    const done = test.start();
    await vi.waitFor(() => expect(test.summaryGenerate).toHaveBeenCalledExactlyOnceWith(1, 'l1'));
    test.run.requested = true;
    test.run.cancelJob?.();
    expect(test.summaryCancel).toHaveBeenCalledOnce();
    late.resolve('Late L1');
    expect(await done).toEqual({ targetIds: [1], compressedIds: [1], folderSummaryCommitted: false });
    expect(test.summaryGenerate).toHaveBeenCalledOnce();
    expect(test.summaryCommit).not.toHaveBeenCalled();
    expect(test.summaryLine).toHaveBeenCalledExactlyOnceWith(1, false);
    expect(test.run.cancelJob).toBeUndefined();
  });

  it('waits for a commit already sent after Cancel', async () => {
    const test = setup([alpha]);
    const commit = deferred<void>();
    test.summaryCommit.mockImplementationOnce(() => commit.promise);
    const completed = vi.fn();
    const done = test.start().then(completed);
    await vi.waitFor(() => expect(test.summaryCommit).toHaveBeenCalledOnce());
    test.run.requested = true;
    test.run.cancelJob?.();
    await Promise.resolve();
    expect(completed).not.toHaveBeenCalled();
    commit.resolve();
    await done;
    expect(test.summaryGenerate.mock.calls).toEqual([[1, 'l1']]);
  });

  it('ignores an overview that arrives after Cancel', async () => {
    const test = setup([beta]);
    const overview = deferred<string>();
    test.summaryGenerate.mockResolvedValueOnce('Section').mockReturnValueOnce(overview.promise);
    const done = test.start();
    await vi.waitFor(() => expect(test.summaryGenerate).toHaveBeenLastCalledWith(9, 'l3_overview', '# Beta\n\nSection'));
    test.run.requested = true;
    test.run.cancelJob?.();
    overview.resolve('Overview');
    expect(await done).toEqual({ targetIds: [2], compressedIds: [], folderSummaryCommitted: false });
    expect(test.summaryCancel).toHaveBeenCalledOnce();
    expect(test.summaryCommit).not.toHaveBeenCalled();
  });

  it('abandons the correct part after a conversion failure and continues other levels', async () => {
    const test = setup([alpha]);
    test.summaryGenerate.mockImplementation(async (_id, part) => part === 'l1' || part === 'l3_section' ? '' : 'Valid');
    expect(await test.start()).toEqual({ targetIds: [1], compressedIds: [1], folderSummaryCommitted: false });
    expect(test.summaryAbandon.mock.calls).toEqual([[1, 'l1'], [9, 'l3']]);
    expect(test.summaryCommit).toHaveBeenCalledExactlyOnceWith(1, 'l2', noteDocument(heading(alpha.name), paragraph('Valid')));
  });

  it('abandons L3 when individually valid documents exceed the combined size limit', async () => {
    const test = setup([beta]);
    const markdown = '```\n' + 'a'.repeat(noteDocumentByteLimit / 2) + '\n```';
    test.summaryGenerate.mockResolvedValue(markdown);
    expect(await test.start()).toEqual({ targetIds: [2], compressedIds: [], folderSummaryCommitted: false });
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
    expect(test.run.cancelJob).toBeUndefined();
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
