import { generateText } from '@tiptap/core';
import { ChaError, type NoteItem, type NotesClient, type SummaryTarget } from '../api/client';
import { noteDocumentByteLength, noteDocumentByteLimit } from './documentBytes';
import { notesExtensions } from './editor';
import { markdownNoteDocument, summaryConversionError } from './summaryImport';
import { compareNoteSiblings } from './tree';

type Section = { name: string; text: string; blocks: unknown[] };

// Headings are built from names, so Markdown characters in a name stay literal.
function heading(text: string) {
  return { type: 'heading', attrs: { level: 1 }, content: [{ type: 'text', text }] };
}

function titledDocument(title: string, markdown: string, sections: Section[] = []) {
  const content: unknown[] = [heading(title), ...markdownNoteDocument(markdown).content as unknown[]];
  for (const section of sections) {
    content.push(heading(section.name));
    for (const block of section.blocks) content.push(block);
  }
  const document = { type: 'doc', content };
  if (noteDocumentByteLength(document) > noteDocumentByteLimit) throw new Error(summaryConversionError);
  return document;
}

export async function runSummaryBatch(
  client: NotesClient,
  source: NoteItem,
  admitted: SummaryTarget[],
  buildFolderSummary: boolean,
  view: {
    checkContext(): void;
    commit: NotesClient['summaryCommit'];
  },
): Promise<void> {
  const targets = admitted.map(target => ({ ...target, kind: 'note' as const })).sort(compareNoteSiblings);
  const due = targets.filter(target => target.levels.length > 0);
  const failedL3s = new Set<number>();

  async function makeLevel({ id, name }: SummaryTarget, part: 'l1' | 'l2' | 'l3_section') {
    const markdown = await client.summaryGenerate(id, part);
    view.checkContext();
    let document;
    try {
      document = part === 'l2' ? titledDocument(name, markdown) : markdownNoteDocument(markdown);
    } catch (failure) {
      await client.summaryAbandon(id, part);
      throw failure;
    }
    await view.commit(id, part, document);
  }

  // Individual failures are recorded in Events. A lost vault or connection stops the line.
  async function step(work: () => Promise<void>) {
    view.checkContext();
    let succeeded = true;
    try {
      await work();
    } catch (failure) {
      if (failure instanceof ChaError && ['vault_changed', 'server_stopping', 'application_unavailable'].includes(failure.code))
        throw failure;
      succeeded = false;
    }
    view.checkContext();
    return succeeded;
  }

  // Keep each note's steps in order so the provider can reuse its cached prefix.
  let next = 0;
  async function line() {
    while (next < due.length) {
      const target = due[next++];
      const { id } = target;
      await step(() => client.summaryLine(id, false));
      if (target.levels.includes('compressed')) await step(async () => {
        const text = await client.summaryGenerate(id, 'compressed');
        view.checkContext();
        await view.commit(id, 'compressed', text);
      });
      if (target.levels.includes('l1')) await step(() => makeLevel(target, 'l1'));
      if (target.levels.includes('l2')) await step(() => makeLevel(target, 'l2'));
      if (target.levels.includes('l3') && !await step(() => makeLevel(target, 'l3_section'))) failedL3s.add(id);
      await step(() => client.summaryLine(id, true));
    }
  }
  const settled = await Promise.allSettled(Array.from({ length: Math.min(3, due.length) }, line));
  for (const result of settled) if (result.status === 'rejected') throw result.reason;

  if (!buildFolderSummary) return;
  const failed = targets.find(({ id }) => failedL3s.has(id));
  if (failed) {
    await step(() => client.summaryAbandon(source.id, 'l3', failed.id));
    return;
  }

  const sections: Section[] = [];
  const extensions = notesExtensions();
  for (const { id, name } of targets) {
    const read = await step(async () => {
      const { document_id, document } = await client.getNote(id, 'l3');
      view.checkContext();
      if (document_id === id || !Array.isArray(document.content))
        throw new Error('The saved L3 is unavailable.');
      const text = generateText(document, extensions, { blockSeparator: '\n\n' });
      sections.push({ name, text, blocks: document.content });
    });
    if (!read) {
      await step(() => client.summaryAbandon(source.id, 'l3', id));
      return;
    }
  }
  const text = sections.map(({ name, text }) => `# ${name}\n\n${text}`).join('\n\n');
  if (new TextEncoder().encode(text).length > noteDocumentByteLimit) {
    await step(() => client.summaryAbandon(source.id, 'l3'));
    return;
  }
  await step(async () => {
    const overview = await client.summaryGenerate(source.id, 'l3_overview', text);
    view.checkContext();
    let document;
    try {
      document = titledDocument(source.name, overview, sections);
    } catch {
      await client.summaryAbandon(source.id, 'l3');
      return;
    }
    await view.commit(source.id, 'l3', document);
  });
}
