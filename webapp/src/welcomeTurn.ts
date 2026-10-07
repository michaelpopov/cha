// Tracks one Welcome answer across session snapshots. An answer can start and
// end between two snapshots, so a submission is remembered as pending.
export interface WelcomeTurn {
  key: string;
  active: boolean;
  pending: boolean;
}

// Applies one Welcome snapshot. `ended` is true once for each finished answer.
export function welcomeSnapshot(
  prior: WelcomeTurn | null,
  key: string,
  active: boolean,
): { turn: WelcomeTurn; ended: boolean } {
  const same = prior?.key === key;
  return {
    turn: { key, active, pending: active && same ? prior.pending : false },
    ended: same && !active && (prior.active || prior.pending),
  };
}

// Records a sent message or Stop, so that the next idle snapshot ends the answer.
export function welcomeSubmission(prior: WelcomeTurn | null, key: string): WelcomeTurn {
  return { key, active: prior?.key === key ? prior.active : false, pending: true };
}
