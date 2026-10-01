declare module 'codec-parser' {
  export default class CodecParser {
    constructor(mimeType: string, options?: { enableFrameCRC32?: boolean });
    parseChunk(bytes: Uint8Array): IterableIterator<unknown>;
    flush(): IterableIterator<unknown>;
  }
}

declare module 'mse-audio-wrapper' {
  export default class MSEAudioWrapper {
    constructor(mimeType: string, options: {
      codec: 'mpeg'; minFramesPerSegment: number; minBytesPerSegment: number;
    });
    iterator(frames: unknown[]): IterableIterator<Uint8Array<ArrayBuffer>>;
  }
}
