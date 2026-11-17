import type { VoiceInputUsage } from './api/client';

export interface VoiceInputRecording extends VoiceInputUsage {
  submittedSeconds: number;
}

export function snapshotVoiceInputUsage(recordings: readonly VoiceInputRecording[] = []) {
  const used = recordings
    .filter(({ audio_seconds, submittedSeconds }) => audio_seconds > submittedSeconds)
    .map((recording) => ({ recording, total: recording.audio_seconds }));
  const usage = used.map(({ recording, total }) => ({
    provider: recording.provider,
    model: recording.model,
    audio_seconds: total - recording.submittedSeconds,
  }));
  return {
    usage: usage.length ? usage : undefined,
    acknowledge() {
      for (const { recording, total } of used) recording.submittedSeconds = total;
    },
  };
}
