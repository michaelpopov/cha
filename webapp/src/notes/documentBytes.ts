export const noteDocumentByteLimit = 12_582_912;

export function noteDocumentByteLength(document: unknown): number {
  return new TextEncoder().encode(JSON.stringify(document)).length;
}

export function paddedNoteDocument(base: unknown, text: string): unknown {
  const document = JSON.parse(JSON.stringify(base)) as {
    content: Array<{ content: Array<{ text: string }> }>;
  };
  document.content[0].content[0].text = text;
  return document;
}
