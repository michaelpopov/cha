const listeners = new Set<(playing: boolean) => void>();
let activePlayers = 0;
let resumeTimer: ReturnType<typeof setTimeout> | undefined;

// Allow speaker output and room echo to settle before recording again.
const echoTailMs = 400;

function notify(playing: boolean): void {
  for (const listener of listeners) listener(playing);
}

// Subscribe before connecting the microphone, including during existing playback.
export function onSpeechPlaybackChange(listener: (playing: boolean) => void): () => void {
  listeners.add(listener);
  listener(activePlayers > 0 || resumeTimer !== undefined);
  return () => { listeners.delete(listener); };
}

export function beginSpeechPlayback(): () => void {
  activePlayers += 1;
  if (resumeTimer !== undefined) {
    clearTimeout(resumeTimer);
    resumeTimer = undefined;
  } else if (activePlayers === 1) {
    notify(true);
  }
  let ended = false;
  return () => {
    if (ended) return;
    ended = true;
    activePlayers -= 1;
    if (activePlayers > 0) return;
    resumeTimer = setTimeout(() => {
      resumeTimer = undefined;
      notify(false);
    }, echoTailMs);
  };
}
