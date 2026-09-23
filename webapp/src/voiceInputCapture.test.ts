import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

const workletSource = readFileSync(
  join(dirname(fileURLToPath(import.meta.url)), 'voiceInputCapture.worklet.js'),
  'utf8',
);

interface CapturePort {
  onmessage: ((event: MessageEvent) => void) | null;
  postMessage(data: { type: string; samples?: Int16Array }): void;
}

interface CaptureProcessor {
  port: CapturePort;
  process(inputs: Array<ArrayLike<number>[]>): boolean;
}

function loadProcessor(): {
  processor: CaptureProcessor;
  messages: Array<{ type: string; samples?: Int16Array }>;
} {
  const messages: Array<{ type: string; samples?: Int16Array }> = [];
  class AudioWorkletProcessor {
    port: CapturePort = {
      onmessage: null,
      postMessage: (data) => { messages.push(data); },
    };
  }
  let Processor!: new () => CaptureProcessor;
  new Function('AudioWorkletProcessor', 'registerProcessor', workletSource)(
    AudioWorkletProcessor,
    (_name: string, ctor: new () => CaptureProcessor) => { Processor = ctor; },
  );
  return { processor: new Processor(), messages };
}

describe('voice capture worklet', () => {
  it('downmixes to mono PCM16 and posts 100 ms batches', () => {
    const { processor, messages } = loadProcessor();
    const frames = 1600;
    const left = new Float32Array(frames);
    const right = new Float32Array(frames);
    left[0] = 1;
    right[0] = 1;
    left[1] = -1;
    right[1] = -1;
    left[2] = 2;
    right[2] = 2;
    processor.process([[left, right]]);
    expect(messages).toHaveLength(1);
    expect(messages[0]?.type).toBe('batch');
    expect(messages[0]?.samples).toBeInstanceOf(Int16Array);
    expect(messages[0]?.samples).toHaveLength(1600);
    expect(messages[0]?.samples?.[0]).toBe(32767);
    expect(messages[0]?.samples?.[1]).toBe(-32768);
    expect(messages[0]?.samples?.[2]).toBe(32767);
    expect(messages[0]?.samples?.[3]).toBe(0);
  });

  it('holds one partial batch and flushes it', () => {
    const { processor, messages } = loadProcessor();
    const frames = new Float32Array(100);
    frames.fill(0.5);
    expect(processor.process([[frames]])).toBe(true);
    expect(messages).toHaveLength(0);
    processor.port.onmessage?.(new MessageEvent('message', { data: { type: 'flush' } }));
    expect(messages.map(({ type, samples }) => [type, samples?.length])).toEqual([
      ['flush-batch', 100],
      ['flushed', undefined],
    ]);
    expect(processor.process([[frames]])).toBe(false);
    expect(messages).toHaveLength(2);
  });
});
