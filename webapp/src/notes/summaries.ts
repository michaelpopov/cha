import type { NoteItem, NotesClient, SummaryTarget } from '../api/client';
import type { NoteCompressionProgress } from '../components/NoteCompressionDialog';
import type { CompressionCancellation } from './compression';
import { noteDocumentByteLength, noteDocumentByteLimit } from './documentBytes';
import { markdownNoteDocument, summaryConversionError } from './summaryImport';
import { compareNoteSiblings } from './tree';

type Section = { name: string; markdown: string };

// Headings are built from names, so Markdown characters in a name stay literal.
function heading(text: string) {
  return { type: 'heading', attrs: { level: 1 }, content: [{ type: 'text', text }] };
}

function titledDocument(title: string, markdown: string, sections: Section[] = []) {
  const content: unknown[] = [heading(title), ...markdownNoteDocument(markdown).content as unknown[]];
  for (const section of sections) {
    content.push(heading(section.name), ...markdownNoteDocument(section.markdown).content as unknown[]);
  }
  const document = { type: 'doc', content };
  if (noteDocumentByteLength(document) > noteDocumentByteLimit) throw new Error(summaryConversionError);
  return document;
}

export async function summarizeAndWait(
  client: NotesClient,
  source: NoteItem,
  run: CompressionCancellation,
  checkContext: () => void,
  onProgress: (progress: NoteCompressionProgress) => void,
): Promise<{ targetIds: number[]; compressedIds: number[]; folderSummaryCommitted: boolean }> {
  const result = { targetIds: [] as number[], compressedIds: [] as number[], folderSummaryCommitted: false };

  let cancellation: Promise<void> | undefined;
  run.cancelJob = () => {
    cancellation = client.summaryCancel().catch(() => {});
  };
  const folder = source.kind === 'folder';

  async function makeLevel({ id, name }: SummaryTarget, part: 'l1' | 'l2') {
    const markdown = await client.summaryGenerate(id, part);
    if (run.requested) return;
    checkContext();
    let document;
    try {
      document = part === 'l2' ? titledDocument(name, markdown) : markdownNoteDocument(markdown);
    } catch {
      await client.summaryAbandon(id, part);
      return;
    }
    await client.summaryCommit(id, part, document);
  }

  try {
    const plan = await client.summaryPlan(source.id);
    const targets = plan.notes.map(target => ({ ...target, kind: 'note' as const })).sort(compareNoteSiblings);
    result.targetIds = targets.map(target => target.id);
    let makeL3 = folder && targets.length > 0;
    // Sections keep their place in tree order, whichever line finishes first.
    const sections: (Section | undefined)[] = targets.map(() => undefined);

    async function compress({ id }: SummaryTarget) {
      if (await client.summaryCompress(id)) result.compressedIds.push(id);
    }

    async function makeSection({ id, name }: SummaryTarget, index: number) {
      if (!makeL3) return;
      try {
        sections[index] = { name, markdown: await client.summaryGenerate(id, 'l3_section') };
      } catch {
        makeL3 = false;
      }
    }

    // A failed step does not stop its line. The backend has already recorded the event.
    // A vault change stops the line.
    async function step(work: () => Promise<void>) {
      if (run.requested) return;
      checkContext();
      try {
        await work();
      } catch {
        // Recorded by the backend.
      }
    }

    // A line makes the steps of one note in order, so later steps can reuse the
    // provider's cached prefix. Up to three lines run at the same time.
    let next = 0;
    async function line() {
      while (next < targets.length && !run.requested) {
        const index = next++;
        const target = targets[index];
        checkContext();
        onProgress({
          message: `Summarizing “${target.name}”…`,
          count: folder ? `Note ${index + 1} of ${targets.length}` : undefined,
          cancelEnabled: true,
        });
        await step(() => client.summaryLine(target.id, false));
        if (folder && !target.has_compressed) await step(() => compress(target));
        if (!folder || !target.has_l1) await step(() => makeLevel(target, 'l1'));
        if (!folder || !target.has_l2) await step(() => makeLevel(target, 'l2'));
        await step(() => makeSection(target, index));
        await step(() => client.summaryLine(target.id, true));
      }
    }
    await Promise.allSettled(Array.from({ length: Math.min(3, targets.length) }, line));
    checkContext();

    if (!makeL3 || run.requested || sections.includes(undefined)) return result;
    const message = 'Writing the Summary note…';
    onProgress({ message, cancelEnabled: true });
    const complete = sections as Section[];
    const text = complete.map(({ name, markdown }) => `# ${name}\n\n${markdown}`).join('\n\n');
    try {
      const overview = await client.summaryGenerate(source.id, 'l3_overview', text);
      if (run.requested) return result;
      checkContext();
      let document;
      try {
        document = titledDocument(source.name, overview, complete);
      } catch {
        await client.summaryAbandon(source.id, 'l3');
        return result;
      }
      onProgress({ message, cancelEnabled: false });
      await client.summaryCommit(source.id, 'l3', document);
      result.folderSummaryCommitted = true;
    } catch {
      // The backend reports failures from generation, commit, and abandon.
    } finally {
      checkContext();
    }
    return result;
  } finally {
    run.cancelJob = undefined;
    await cancellation;
  }
}
