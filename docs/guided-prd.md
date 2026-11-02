# Model-guided sessions

Status: Proposed PRD

Date: November 1, 2026

## Purpose

In a normal session, the user leads: the user writes a prompt, and the model answers it. In a model-guided session, the model leads. The user switches the session to this mode with the `/guided` command and gives a topic and guidelines. The model then gives content in parts of limited length. After each part, the model stops and gives control to the user. The user replies with new information or with a short phrase such as "continue". The model then gives the next part.

Examples:

- The model acts as a psychologist. It interviews the user about the user's childhood and collects information for a later evaluation.
- The user sets the conditions of a virtual world. The model describes what occurs around the user and lets the user choose a direction. The user can also change the conditions of the world.

This feature is a small extension of the current chat. It adds one command, `/guided`, and uses the existing model requests. It adds no screen, control, label, or setting.

## Scope and simple defaults

- The target is the desktop app on macOS and Windows. ChaWeb uses the same session logic. This version makes no ChaWeb changes and does not block the mode in ChaWeb.
- The mode applies only to stored sessions. The Welcome session is always a normal session.
- One character narrates. Roles for several characters are outside this version.
- The user starts the mode with the `/guided` command. Jev does not take part in this decision. The mode works the same with and without Jev.
- The length of a part is a soft limit in the model instruction. CHA never cuts model output. The provider's existing `max_tokens` setting is the safety stop.
- A model-guided session is not expected to be longer than a normal session. It needs no special handling of long context.
- The session display, transcript, storage, Markdown mirror, **remove turn** control, automatic audio, and dictation stay as they are.

## User flow

1. The user opens a new session and selects the character in the target selector.
2. The user sends `/guided`. The session becomes a model-guided session. See [The `/guided` command](#the-guided-command).
3. The user writes the first prompt. The prompt gives the topic and the guidelines. Example: "Act as a psychologist. Interview me about my childhood." The first answer of the model is already the first part.
4. The user reads or listens to the part. Then the user sends a message:
   - A short phrase with no new content, for example "continue", "go on", or "let's move". The model continues from where it stopped.
   - A message with new content, for example an answer, a choice of direction, or a change of the world's conditions. The model uses the content and continues. A separate "continue" is not necessary.
5. Step 4 repeats. The user can stop at any time: leave the session, use **Stop**, or close CHA. Later, the user can reopen the session and continue.

## The `/guided` command

The `/guided` command tells CHA that the session must switch to the model-guided mode. Only this command starts the mode. CHA makes no Jev request and no other model call for this decision.

- The user can send `/guided` at any time in a stored session: before the first prompt or after earlier turns. Each character request after the command gets the model instruction. Earlier answers do not change.
- The command adds nothing to the transcript and shows no message when it succeeds.
- `/guided` in a session that is already model-guided changes nothing.
- `/guided` takes no argument. With an argument, CHA shows "Usage: /guided" and does not change the mode.
- In Welcome, CHA shows "Guided mode is not available in Welcome" and does not change the mode.
- During generation, the existing busy notice applies, as for the other commands.
- The list of commands in the existing "Unknown command" notice includes `/guided`.
- Jev, `@Name`, `/mcast`, and self-notes keep their current behavior. They do not start or end the mode.
- `/clear` ends the mode. It is the only way to end it.

A session is a normal session until the user sends `/guided`. This includes the sessions from before this change.

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
| The user removes a turn with the existing control, also the last remaining turn | The mode does not change. The user sends a message to get a new part. |
| The user uses `/clear` | The mode ends, also when the session has no messages. The session is a normal session until the user sends `/guided` again. |
| The user closes and reopens the session, or restarts CHA | The mode stays. |
| The user uses **Stop** during a part | The current Stop behavior applies. The next message continues the session. |
| A part reaches the provider's `max_tokens` | The current behavior applies. CHA does not cut or extend the part. |

## Voice and hands-free use

A model-guided session works with automatic audio and dictation, like any other session.

- With automatic audio, CHA plays each part as it does for other answers.
- With hands-free dictation, the user speaks a reply or a short phrase and then the send phrase, for example "go on, over to you". As now, the send phrase alone sends nothing.
- The user types `/guided`. This feature adds no voice command, phrase, or setting.

## Unchanged behavior

- The transcript shows the user's messages, including "continue", as normal messages.
- The instruction goes only to character answers. Session naming, conversation summaries, and Jev requests do not change.
- The rules for `@Name`, `/mcast`, self-notes, and the cover boundary do not change.
- Storage, the Markdown mirror, and vault export work as now.
- There is no visible UI change: no indicator, label, button, or setting. The only new text is in the notices of the `/guided` command and in the list of commands.

## Acceptance criteria

1. In a new session, the user sends `/guided` and then the psychologist example. The first answer is one part of about 2–4 paragraphs, and then the model waits for the user.
2. After `/guided`, the virtual world example gives a description and waits. A choice of direction, or a change of the world's conditions, changes the next part.
3. "continue", "go on", and "let's move" each give the next part. The part continues from where the previous part stopped.
4. Without `/guided`, the psychologist example and "Guide me through the history of Rome" each give a normal session. Their requests do not contain the instruction.
5. `/guided` works the same with Jev configured and with Jev disabled, and with prompts that start with `@Name` or `/mcast`. The Jev requests do not change.
6. In a session with earlier turns, the user sends `/guided`. The next answers are parts. Earlier answers do not change.
7. `/guided` with an argument shows "Usage: /guided". `/guided` in Welcome shows "Guided mode is not available in Welcome". In both cases, the mode does not change.
8. A first prompt that asks for one paragraph at a time gives parts of one paragraph.
9. This feature adds no truncation. CHA never cuts a part. A part that reaches the provider's `max_tokens` follows the current provider-limit and error behavior.
10. After the user reopens the session or restarts CHA, the next message continues the model-guided session.
11. In a forum with several characters, a later message to another character also gets the instruction.
12. After `/guided` and the first prompt, with automatic audio and hands-free dictation, the user hears each part and says "go on, over to you" to get the next part, with no keyboard.
13. In a model-guided session, the removal of turns, also the last remaining turn, keeps the mode. `/clear` ends the mode: the next requests do not contain the instruction until the user sends `/guided` again.
14. A session from before this change is a normal session until the user sends `/guided`.
15. No screen shows a new control, label, or indicator.

## Deferred

- A visible indicator of the mode, or a way to end it without `/clear`.
- Several narrating characters with separate roles.
- ChaWeb changes and ChaWeb tests.
- Special handling of long sessions.
- An editable instruction, or part lengths for each character.
