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
): Promise<{ targetIds: number[]; folderSummaryCommitted: boolean }> {
  const result = { targetIds: [] as number[], folderSummaryCommitted: false };

  let cancellation: Promise<void> | undefined;
  run.cancelJob = () => {
    cancellation = client.summaryCancel().catch(() => {});
  };
  const folder = source.kind === 'folder';
  const sections: Section[] = [];

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

    async function makeSection({ id, name }: SummaryTarget) {
      try {
        sections.push({ name, markdown: await client.summaryGenerate(id, 'l3_section') });
      } catch {
        makeL3 = false;
      }
    }

    for (const [index, target] of targets.entries()) {
      if (run.requested) break;
      checkContext();
      onProgress({
        message: `Summarizing “${target.name}”…`,
        count: folder ? `Note ${index + 1} of ${targets.length}` : undefined,
        cancelEnabled: true,
      });
      const calls = [];
      if (!folder || !target.has_l1) calls.push(makeLevel(target, 'l1'));
      if (!folder || !target.has_l2) calls.push(makeLevel(target, 'l2'));
      if (makeL3) calls.push(makeSection(target));
      await Promise.allSettled(calls);
    }

    if (!makeL3 || run.requested) return result;
    checkContext();
    const message = 'Writing the Summary note…';
    onProgress({ message, cancelEnabled: true });
    const text = sections.map(({ name, markdown }) => `# ${name}\n\n${markdown}`).join('\n\n');
    try {
      const overview = await client.summaryGenerate(source.id, 'l3_overview', text);
      if (run.requested) return result;
      checkContext();
      let document;
      try {
        document = titledDocument(source.name, overview, sections);
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
