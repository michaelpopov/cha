# Model-guided sessions

Status: Proposed PRD

Date: November 1, 2026

## Purpose

In a normal session, the user leads: the user writes a prompt, and the model answers it. In a model-guided session, the model leads. The user gives a topic and guidelines in the first prompt. The model then gives content in parts of limited length. After each part, the model stops and gives control to the user. The user replies with new information or with a short phrase such as "continue". The model then gives the next part.

Examples:

- The model acts as a psychologist. It interviews the user about the user's childhood and collects information for a later evaluation.
- The user sets the conditions of a virtual world. The model describes what occurs around the user and lets the user choose a direction. The user can also change the conditions of the world.

This feature is a small extension of the current chat. It uses the existing Jev request and the existing model requests. It adds no screen, control, label, or setting.

## Scope and simple defaults

- The target is the desktop app on macOS and Windows. ChaWeb uses the same session logic. This version makes no ChaWeb changes and does not block the mode in ChaWeb.
- The mode applies only to stored sessions. The Welcome session is always a normal session.
- One character narrates. Roles for several characters are outside this version.
- Jev must be configured in **Settings → Session settings → Recipient detection** to detect a guided session at the first prompt. Without Jev, new sessions are normal sessions. Disabling Jev does not change a session that is already model-guided.
- The length of a part is a soft limit in the model instruction. CHA never cuts model output. The provider's existing `max_tokens` setting is the safety stop.
- A model-guided session is not expected to be longer than a normal session. It needs no special handling of long context.
- The session display, transcript, storage, Markdown mirror, **remove turn** control, automatic audio, and dictation stay as they are.

## User flow

1. The user opens a new session and selects the character in the target selector.
2. The user writes the first prompt without `@Name` and without `/mcast`. The prompt gives the topic and the guidelines. In the user's own words, it also asks the model to lead the conversation. Example: "Act as a psychologist. Interview me about my childhood. You lead the conversation."
3. CHA asks Jev if the user wants a model-guided session. See [Detection](#detection).
4. If Jev answers yes, the session becomes a model-guided session. The first answer of the model is already the first part.
5. The user reads or listens to the part. Then the user sends a message:
   - A short phrase with no new content, for example "continue", "go on", or "let's move". The model continues from where it stopped.
   - A message with new content, for example an answer, a choice of direction, or a change of the world's conditions. The model uses the content and continues. A separate "continue" is not necessary.
6. Step 5 repeats. The user can stop at any time: leave the session, use **Stop**, or close CHA. Later, the user can reopen the session and continue.

## Detection

CHA adds one question to the existing Jev request for the first prompt: does the user want a model-guided session? CHA makes no second Jev request and no other model call for this decision.

- A prompt is the first prompt when the session has no earlier user message. This is true for a new session, after `/clear`, and after the user removes all turns.
- Jev checks only the first prompt. The decision for the first prompt sets the mode for the full session. Later prompts do not change the mode.
- The answer is yes only when the user clearly asks the model to lead the conversation, for example with "Guide me", "Lead the story", "Let me follow you", or "Take the lead". The topic alone is not sufficient. A request for one long text gives a normal session.
- CHA writes the decision to the diagnostic log, like the other Jev decisions.

| First prompt | Decision |
| --- | --- |
| "Act as a psychologist. Interview me about my childhood, one question at a time. You lead." | Model-guided |
| "You are the narrator of a world with these rules: … Guide me through this world." | Model-guided |
| "Guide me through the history of Rome." | Model-guided |
| "Write a long story about a dragon." | Normal |
| "What were the main causes of the First World War?" | Normal |

The session is a normal session in these cases:

- The session is Welcome.
- Jev is not configured.
- The Jev request fails or times out, or its answer is missing or not valid.
- The first prompt starts with `@Name` or `/mcast`. These prompts do not go to Jev now, and this feature keeps that rule.
- CHA records the first message as a self-note.
- The session already had user messages before this change.

If the detection is wrong, the user starts a new session with a clearer first prompt.

## Model instruction

In a model-guided session, CHA adds a fixed instruction to each request that the session sends to a character, including the first request. The text is part of the application, and the user cannot edit it. The technical design decides where the request carries the text.

Draft text for review:

```text
The user asked you to lead this conversation. Follow the topic and the
guidelines that the user gave. Give the content in parts. Make each answer
one part: usually 2 to 4 paragraphs, and never more than 6 paragraphs. A part
can be shorter, for example one question. End each part at a natural stop,
then stop and wait for the user. Do not write the user's words, answers, or
decisions. If the user's message has no new content, for example "continue"
or "go on", continue from where you stopped. If the message has new content,
use it and continue. The user's own guidelines, at the start or later, take
precedence over these rules, including the length of a part.
```

The instruction sets no fixed form for the end of a part. The model can end a part with a question, with choices, or with no question, as the content and the user's guidelines require.

The user's text controls the session. In any message, the user can change the topic, the direction, the conditions of a world, or the length of the parts, and the model obeys. CHA does not process such changes itself.

## Session rules

| Event | Behavior |
| --- | --- |
| The user sends a later message to another character, or uses `/mcast` | Each character that answers in the session gets the same instruction. |
| The user removes a turn with the existing control, and user messages remain | The mode does not change. The user sends a message to get a new part. |
| The user removes the last remaining turn | The topic and the guidelines are removed with it. The next prompt is a first prompt again. |
| The user uses `/clear` | The next prompt is a first prompt again. |
| The user closes and reopens the session, or restarts CHA | The mode stays. |
| The user uses **Stop** during a part | The current Stop behavior applies. The next message continues the session. |
| A part reaches the provider's `max_tokens` | The current behavior applies. CHA does not cut or extend the part. |

## Voice and hands-free use

A model-guided session works with automatic audio and dictation, like any other session.

- With automatic audio, CHA plays each part as it does for other answers.
- With hands-free dictation, the user speaks a reply or a short phrase and then the send phrase, for example "go on, over to you". As now, the send phrase alone sends nothing.
- This feature adds no voice command, phrase, or setting.

## Unchanged behavior

- The transcript shows the user's messages, including "continue", as normal messages.
- The instruction goes only to character answers. Session naming, conversation summaries, and the other Jev questions do not change.
- The rules for `@Name`, `/mcast`, self-notes, and the cover boundary do not change.
- Storage, the Markdown mirror, and vault export work as now.
- There is no visible UI change: no indicator, label, button, or setting.

## Acceptance criteria

1. With Jev configured, a new session starts with the psychologist example. The first answer is one part of about 2–4 paragraphs, and then the model waits for the user.
2. The virtual world example gives a description and waits. A choice of direction, or a change of the world's conditions, changes the next part.
3. "continue", "go on", and "let's move" each give the next part. The part continues from where the previous part stopped.
4. "Write a long story about a dragon" and an ordinary question each give a normal session. Their requests do not contain the instruction.
5. Each of these gives a normal session that works as now: the Welcome session, Jev disabled, a failed or timed-out Jev request, a first prompt with `@Name` or `/mcast`, and a first message recorded as a self-note.
6. In a normal session, a later prompt that asks the model to lead does not change the mode. After `/clear`, Jev checks the next prompt.
7. A first prompt that asks for one paragraph at a time gives parts of one paragraph.
8. This feature adds no truncation. CHA never cuts a part. A part that reaches the provider's `max_tokens` follows the current provider-limit and error behavior.
9. After the user reopens the session or restarts CHA, the next message continues the model-guided session.
10. In a forum with several characters, a later message to another character also gets the instruction.
11. With automatic audio and hands-free dictation, the user hears each part and says "go on, over to you" to get the next part, with no keyboard.
12. In a session with earlier turns, the user removes the last turn and sends a message again. The mode stays, and the model gives a new part. In a session with only one turn, the user removes that turn. The next prompt is a first prompt, and its decision sets the mode again.
13. A session that had user messages before this change stays a normal session.
14. The diagnostic log shows the decision for each first prompt that Jev checks.
15. No screen shows a new control, label, or indicator.

## Deferred

- A visible indicator of the mode, or a manual control to start or end it.
- Detection without Jev, or detection on later prompts.
- Several narrating characters with separate roles.
- ChaWeb changes and ChaWeb tests.
- Special handling of long sessions.
- An editable instruction, or part lengths for each character.
