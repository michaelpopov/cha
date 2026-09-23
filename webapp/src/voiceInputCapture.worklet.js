const batchSamples = 1600;

function clampSample(sample) {
  const clamped = Math.max(-1, Math.min(1, sample));
  const scaled = clamped < 0 ? clamped * 32768 : clamped * 32767;
  return Math.max(-32768, Math.min(32767, Math.round(scaled)));
}

class ChaVoiceCaptureProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.partial = new Int16Array(batchSamples);
    this.count = 0;
    this.closed = false;
    this.port.onmessage = (event) => {
      if (!event.data || event.data.type !== 'flush' || this.closed) return;
      this.closed = true;
      if (this.count > 0) {
        const samples = this.partial.slice(0, this.count);
        this.count = 0;
        this.port.postMessage({ type: 'flush-batch', samples }, [samples.buffer]);
      }
      this.port.postMessage({ type: 'flushed' });
    };
  }

  push(sample) {
    this.partial[this.count] = clampSample(sample);
    this.count += 1;
    if (this.count < batchSamples) return;
    const samples = this.partial.slice(0, batchSamples);
    this.count = 0;
    this.port.postMessage({ type: 'batch', samples }, [samples.buffer]);
  }

  process(inputs) {
    if (this.closed) return false;
    const channels = inputs[0];
    const first = channels && channels[0];
    if (!first || first.length === 0) return true;
    const channelCount = channels.length;
    for (let frame = 0; frame < first.length; frame += 1) {
      let sum = 0;
      for (let channel = 0; channel < channelCount; channel += 1) {
        sum += channels[channel][frame] || 0;
      }
      this.push(sum / channelCount);
    }
    return true;
  }
}

registerProcessor('cha-voice-capture', ChaVoiceCaptureProcessor);
