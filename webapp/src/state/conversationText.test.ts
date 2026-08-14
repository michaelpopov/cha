import { describe, expect, it } from 'vitest';

import { snapshotFixture } from '../test/fixtures';
import { conversationText, hasCopyableConversation } from './conversationText';

describe('conversationText', () => {
  it('copies only visible conversation entries with stable speaker and status markers', () => {
    const snapshot = {
      ...snapshotFixture,
      session_label: 'Architecture review',
      transcript: [
        {
          id: 1, kind: 'notice' as const, participant_id: '', display_name: '',
          addressed_to: '', addressed_to_name: '', text: 'Off-record boundary',
          status: 'complete' as const, created_at: null,
        },
        {
          id: 2, kind: 'human' as const, participant_id: 'guest', display_name: 'Guest',
          addressed_to: 'guide', addressed_to_name: 'Epictetus', text: 'Hello\r\nthere',
          status: 'complete' as const, created_at: null,
        },
        {
          id: 3, kind: 'character' as const, participant_id: 'guide', display_name: 'Epictetus',
          addressed_to: '', addressed_to_name: '', text: 'Working', status: 'streaming' as const,
          created_at: null,
        },
        {
          id: 4, kind: 'error' as const, participant_id: '', display_name: '',
          addressed_to: '', addressed_to_name: '', text: 'Interrupted', status: 'cancelled' as const,
          created_at: null,
        },
      ],
    };

    expect(hasCopyableConversation(snapshot)).toBe(true);
    expect(conversationText(snapshot)).toBe(
      'Session: Architecture review\n'
      + 'Forum: Entrance\n\n'
      + 'Guest -> Epictetus:\nHello\nthere\n\n'
      + 'Epictetus: [in progress]\nWorking\n\n'
      + 'Error: [stopped]\nInterrupted\n',
    );
  });

  it('does not enable copying for notices or empty entries alone', () => {
    expect(hasCopyableConversation({
      ...snapshotFixture,
      transcript: [{
        id: 1, kind: 'notice', participant_id: '', display_name: '', addressed_to: '',
        addressed_to_name: '', text: 'Boundary', status: 'complete', created_at: null,
      }],
    })).toBe(false);
  });
});
