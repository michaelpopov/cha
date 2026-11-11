import { getSchema } from '@tiptap/core';
import { DOMParser as NoteParser, type Node as ProseMirrorNode } from '@tiptap/pm/model';

import { flattenClipboardTable, notesExtensions } from './editor';
import { noteDocumentByteLength, noteDocumentByteLimit } from './documentBytes';
import { notePageHtml } from './pageImport';

export const summaryConversionError = 'The generated summary could not be converted to a note. Save summary again.';

function markdownNoteNode(markdown: string): ProseMirrorNode {
  if (new TextEncoder().encode(markdown).length > noteDocumentByteLimit)
    throw new Error(summaryConversionError);
  const body = document.createElement('div');
  body.innerHTML = notePageHtml({ url: 'file:///', title: '', markdown }, { summary: true });
  for (const table of [...body.querySelectorAll('table')].reverse()) {
    for (const cell of table.querySelectorAll('th, td')) {
      for (const attribute of ['rowspan', 'colspan']) {
        const span = cell.getAttribute(attribute);
        if (span !== null && span !== '1') throw new Error(summaryConversionError);
      }
    }
    flattenClipboardTable(table);
  }
  return NoteParser.fromSchema(getSchema(notesExtensions())).parse(body);
}

function checkedDocument(node: ProseMirrorNode): Record<string, unknown> {
  const result = node.toJSON() as Record<string, unknown>;
  if (noteDocumentByteLength(result) > noteDocumentByteLimit)
    throw new Error(summaryConversionError);
  return result;
}

export function markdownNoteDocument(markdown: string): Record<string, unknown> {
  const parsed = markdownNoteNode(markdown);
  if (!parsed.textContent.trim()) throw new Error(summaryConversionError);
  return checkedDocument(parsed);
}

export function summaryDocument(markdown: string): Record<string, unknown> {
  const parsed = markdownNoteNode(markdown);
  const titles = ['short summary', 'long summary'];
  let chapter = 0;
  let readable = false;
  parsed.forEach(node => {
    if (node.type.name === 'heading' && node.attrs.level === 1) {
      if (node.textContent.toLowerCase() !== titles[chapter] || (chapter > 0 && !readable))
        throw new Error(summaryConversionError);
      chapter++;
      readable = false;
    } else {
      if (chapter === 0) throw new Error(summaryConversionError);
      if (node.type.name !== 'heading' && node.textContent.trim()) readable = true;
    }
  });
  if (chapter !== 2 || !readable) throw new Error(summaryConversionError);
  return checkedDocument(parsed);
}
