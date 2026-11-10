# Skills and scripted methods in CHA

Date — November 10, 2026

Status — Draft PRD for discussion. Notes-based storage reflects the direction
established in the discussion. Runtime, invocation, and execution details below
are proposals to evaluate during technical design. Visible UI changes require
a visual proposal and the user's approval under `AGENTS.md`.

## Purpose

Allow the user to define, run, and adapt personal data-processing methods inside
CHA. A method can read existing notes, ask models to interpret their content,
transform the results with a script, and produce useful notes with references
to the source material.

The initial motivation is a way to read books at several levels of detail.
The user wants to enter through a short overview, follow ideas that attract
their attention, and move into the original text when more context is useful.
Generating these layers is one example of a broader need. The user should be
able to experiment with how information is selected, organized, explained, and
presented without rebuilding CHA for each variation.

CHA is a personal application. The design should grow from a small number of
working methods and reuse the existing Notes editor, storage, providers, and
application operations.

## User experience and value

The user keeps a method beside the material it processes, edits its instructions
in the familiar Notes editor, and runs it on selected content. The result appears
as ordinary notes that the user can read, annotate, edit, and export.

This reduces several recurring costs.

| Current effort | Experience with skills |
| --- | --- |
| Repeatedly explain the same task to an assistant | Save the method once and reuse it with different source material. |
| Change application code to adjust a processing method | Edit prompts, parameters, or the method's script in Notes. |
| Manually move results between several model calls | Run a script that supplies each step with the appropriate input. |
| Reconstruct where a generated statement came from | Follow references carried through the processing steps. |
| Commit to one way of reading a long source | Generate several useful levels of detail and choose where to spend attention. |
| Lose a successful experiment in a conversation | Save the instructions, prompts, and script together as a reusable method. |

For the book workflow, success means the user can begin reading with little
preparation, explore an interesting idea, and return to the place they left.
The usefulness of this experience should be tested with the user on actual
reading material. Compression ratios alone cannot establish that it works.

## Product model

A **skill** is a named collection of instructions and supporting resources for
a task. A skill can include prompts, examples, templates, and a script. Its
instructions explain its purpose, suitable inputs, options, and expected
results.

A **script** expresses the procedure. It can iterate over notes, select inputs,
call a model, validate results, branch according to those results, and assemble
output documents. The model contributes interpretation and generation at the
steps where the method requests them.

CHA supplies the **operations** available to that procedure. Examples include
reading note blocks, making a request through a configured provider, copying a
source excerpt, constructing a reference, and saving a group of output notes.
The available operations determine what a skill can accomplish. A new prompt
or script can combine existing operations in new ways. A new application
capability can still require a CHA code change.

The concept supports several forms of use.

| Form | Example | Execution |
| --- | --- | --- |
| Reusable instructions | Apply a personal checklist when discussing an article | An assistant reads the skill and follows it in the conversation. |
| Scripted model workflow | Build book-reading layers chapter by chapter | The script controls the sequence and calls models for selected steps. |
| Deterministic transformation | Gather selected highlights into an index | The script processes the available data directly. |
| Interactive method | Ask the user what they want to understand, then prepare a reading guide | An assistant conducts the conversation and invokes a saved workflow when its inputs are ready. |

The first delivery should establish scripted processing. Instruction reuse and
interactive invocation can build on the same stored resources after the first
workflow is useful.

## Skills live in Notes

A skill is stored as an ordinary folder with ordinary notes. For example, an
illustrative collection could contain the following resources.

```text
Skills/
└── Book layers/
    ├── Instructions
    ├── generate.lua
    ├── Select passages
    ├── Select essence
    ├── Summarize chapter
    └── Output example
```

The folder name identifies the skill to the user. Resource names identify its
instructions, script, and prompts. The precise recognition convention and
entry-point naming are technical-design decisions. `Skills`, `Instructions`,
and `generate.lua` above illustrate the arrangement.

The saved notes are the source of truth. Execution reads those notes directly.
The design should use their existing save, synchronization, and folder transfer
behavior. Temporary values held while a run executes belong to that run.

Instructions, examples, and prompts use ordinary document formatting. Script
source is stored in a code block, and the runner extracts its exact text,
including indentation and line breaks. Markdown rendering is appropriate for
prompt documents. Code extraction needs a distinct rule that preserves the
script's characters.

The editor already supports stored code blocks and has a regression test for
preserving indentation when a document is reopened. Its visible Code command
currently applies inline formatting. Technical design must account for
comfortable creation and editing of a script block using the existing editor.
Any required control change follows the normal UI approval process.
[Editor test](../webapp/src/notes/editor.test.ts)
[Current editor controls](../webapp/src/components/NotesScreen.tsx)

A skill should refer to its own prompt and template notes relative to its
folder. This allows the folder to be moved or imported without carrying
machine-specific paths. References to external input material are supplied
when the skill runs. Missing required resources produce an error naming the
resource. Unused or obsolete settings follow CHA's existing warning behavior.

Opening, editing, saving, synchronizing, and importing a skill are content
operations. Execution begins through an explicit run request.

## Running and adapting a method

The initial interaction should require only the information the selected method
needs. The user chooses a skill, the source note or folder, and a destination
for its output. A skill can also accept a small set of relevant options, such
as a reading question, output language, or desired depth.

For example, the user could run Book layers on an imported book with the
question “Which ideas help me finish projects?” The question is input to the
selection prompts. The same script can run again with a different question or
with general coverage of the author's argument.

Before execution, CHA saves relevant editor changes through the existing save
path. The runner captures the skill resources it will use so that an edit
during execution takes effect on the next run. A save failure prevents execution
of a version the user believes was saved.

The user can continue reading while processing runs. Progress identifies the
current unit and step, such as a chapter and passage selection. Completion
makes the resulting notes easy to open. Errors identify the affected unit,
step, and actionable cause. Cancellation must work during both a provider call
and script computation.

The proposed default is to create results in a fresh output folder using CHA's
existing naming rules. This makes it possible to compare methods and preserves
the user's edits to earlier results. Updating an existing result requires an
explicitly defined update behavior and target.

The user can adapt a method at several levels.

| Desired change | What the user edits |
| --- | --- |
| Prefer examples, counterarguments, or practical advice | Selection prompts |
| Use a different tone or output language | Generation prompts or run options |
| Change the number or order of processing steps | Script |
| Group results by topic instead of chapter | Script and output instructions |
| Add a review of the generated summary | An additional prompt and script step |
| Explain when the method is useful | Skill instructions |

Prompt changes are accessible to a user who can describe what they want.
Changes to execution logic involve scripting. Example skills and later
AI-assisted authoring can reduce that effort.

## Functional requirements

### R1. Author and maintain skills

The user can create and edit a skill's resources with Notes. The runner can
identify its entry point and check script syntax before starting model work.
An attempt to read a missing resource identifies that resource and the current
step. A minimal working example should make it possible to learn by editing
a real method.

### R2. Select and inspect input

A run accepts a note or folder according to the method's supported inputs.
The method can enumerate the selected folder and, when its behavior calls for
it, its descendants. Enumeration follows a defined order consistent with Notes.
The method distinguishes source notes from generated annotation notes and
previous outputs. The book method processes the imported reading-order notes.

Text is available as structured blocks with references to their source. The
script can obtain plain text for a model while retaining the structure needed
to copy excerpts and build navigation. The input scope is established by the
run request.

### R3. Generate through configured providers

A method uses CHA's configured providers and existing credential handling.
The default can reuse the current Notes provider selection. Any method-specific
provider choice refers to an existing provider configuration.

Each generation can receive a prompt resource, selected input, and relevant
options. Scripts can process structured results as well as generated prose.
Malformed, refused, truncated, or unsuccessful results must reach the workflow
as identifiable outcomes. Oversized input should identify the failing step so
that the method can be adapted to split the source appropriately.

### R4. Compose processing steps

Scripts can express ordinary loops, conditions, grouping, and intermediate
results. A method can make one model call, chain several calls, or complete
using deterministic operations alone. An embedded runtime such as Lua supplies
these language features.

The initial host operations should be derived from the book workflow and one
small deterministic example. Additional operations should follow demonstrated
uses. CHA's existing provider and note services are the starting points.
[Prompt execution](../src/notes/notes_operations.cpp)
[Note operations](../src/notes/notes_service.h)

### R5. Create readable, traceable output

Output consists of ordinary CHA notes and folders using supported formatting.
A method can create several related notes and return an entry note for reading.
Its output can include references to original passages and other generated
notes.

An excerpt operation copies text from a valid source selection. A reference
operation identifies a real destination. These operations provide reusable
checks for workflows that need traceability. Model-generated identifiers and
text selections require validation before they become references or excerpts.

Traceability establishes where an output came from. Evaluating whether a
summary fairly represents its supporting passages remains part of the method's
quality assessment.

### R6. Preserve useful results through failures

A long workflow can publish completed units as it progresses. For the book
method, a unit is one chapter's related outputs. Those outputs and their links
should become available together. A cancellation or failure leaves completed
units readable and reports the incomplete work accurately.

Saving results must respect existing note validation and transaction behavior.
Retrying a save must resolve whether it committed so that it cannot silently
create duplicate output. An explicit new run creates the new result requested
by the user. Restarting work after application shutdown is an explicit run in
the first delivery.

### R7. Handle changes to source material

Processing uses a consistent view of the source for each published unit. If
the source changes or disappears while that unit is being generated, CHA must
identify the conflict before publishing references that would point elsewhere.
The user can rerun the affected unit against the current source.

After generation, following a reference must either reach the intended content
or report that its target has changed or become unavailable. The reference
design must account for source edits, note movement, synchronization, and
folder import. Database row numbers alone need evaluation against those cases.

### R8. Keep execution understandable and controllable

The run identifies its skill, input, destination, progress, and outcome using
existing application patterns where practical. Runtime failures include the
script resource and location when available. A cancellation must remain
effective for a script caught in a computation loop.

The initial script environment exposes the CHA operations required by the
method, with reads scoped to the supplied inputs and skill resources and writes
scoped to the selected output. Provider requests use CHA's normal connection
and credential handling. Source documents enter model calls as task data.
These boundaries are enforced by the host.

A run belongs to the vault and inputs selected when it starts. Closing or
switching that vault must end the run through the existing application lifecycle
so that a delayed result cannot be written into another vault.

### R9. Transfer and reuse methods

Folder export and import carry the skill's editable resources. Relative
references inside the package continue to work after import. An unavailable
provider or required operation produces a clear compatibility error when the
user attempts the relevant run.

The public Agent Skills format may guide naming and future import/export
compatibility. Its resource format supports instructions and optional scripts,
but successful execution also depends on the operations supplied by the host.
Compatibility requirements should be chosen explicitly during design.
[Agent Skills specification](https://agentskills.io/specification)

## Example methods

### Progressive book reading

**Input.** An imported book folder, an optional reading question, and a desired
level of compression.

**Method.** For each source chapter, select useful passages as L2, select a
smaller set of exact phrases or sentences within those passages as L3, and
generate an L4 overview supported by those selections. The script validates
excerpts, creates the related notes, and assembles a book entry note containing
the chapter overviews.

L2 and L3 preserve the author's words. L4 can synthesize them. Compression
percentages are adjustable targets. Selection should preserve necessary
definitions, qualifications, and examples. L3 can omit an L2 passage when it
adds no essential phrase. A summary statement can cite several supporting
selections.

**Output.** A parallel folder of chapter passages and essence notes, plus one
entry note of chapter overviews. Links allow movement to a specific idea at a
deeper level and to the original passage. Returning from an exploration restores
the previous reading location.

**Experience.** The user scans the overview, follows an interesting claim, reads
more context, and returns to the overview. The method can be adjusted for broad
understanding, a particular project, or preservation of examples that make the
ideas easier to understand.

Forte's original approach progressively condenses material the reader has
captured and revisited. This workflow adapts that idea into a prepared reading
aid. Its generated selections express the model's interpretation of importance,
which the user can refine through prompts and their own reading.
[Progressive Summarization](https://fortelabs.com/blog/progressive-summarization-a-practical-technique-for-designing-discoverable-notes/)

CHA currently imports EPUB reading-order files into separate notes. Some books
split a chapter across several files. The first method should describe its use
of those existing note boundaries. Also, ordinary Notes links currently handle
web destinations. Exact navigation between generated and source notes is a
CHA capability this use case requires. Annotation navigation provides an
existing implementation to study.
[EPUB import](../webapp/src/notes/epubImport.ts)
[Annotation navigation](../webapp/src/notes/annotationSource.ts)

### Compare several accounts of a topic

**Input.** A folder containing articles or notes about the same subject and a
question the user wants to investigate.

**Method.** Extract relevant claims from each source, group related claims, and
ask a model to describe agreements, disagreements, and gaps in the supplied
material. Each group retains its source references.

**Output.** A comparison note with links to the passages behind each position.

**Experience.** The user can understand the range of views and inspect the
evidence for a disputed point. Changing the question reuses the same procedure
for a different investigation.

### Build a glossary while reading

**Input.** A technical book or a folder of study notes.

**Method.** Identify important terms, collect the author's definitions and
examples, and combine repeated terms into entries. A model can propose a
plain-language explanation, with a source reference beside it.

**Output.** A glossary organized for lookup and linked to the places where each
term is introduced or used.

**Experience.** The user can recover unfamiliar terminology quickly while
following a longer argument. The selection prompt can favor concepts relevant
to the user's existing knowledge.

### Create a question-based reading guide

**Input.** A chapter and the user's learning question.

**Method.** Generate questions the chapter can help answer, connect each
question to supporting passages, and create a separate answer note. The user
can adjust question difficulty and the balance between explanation and recall.

**Output.** A question note and linked answers with references to the chapter.

**Experience.** The user enters the material through a concrete question and
chooses when to reveal the explanation. Later conversational integration could
conduct a discussion using the same saved method and source material.

### Turn reflection notes into a project review

**Input.** User-selected notes about a project, including saved conversation
summaries when relevant.

**Method.** Extract decisions, unresolved questions, and candidate next actions.
Group repeated items and preserve references to their origin. Prompts distinguish
recorded commitments from suggestions generated during the review.

**Output.** A review note the user can edit into their working plan.

**Experience.** The user revisits scattered thinking without repeatedly
reconstructing the same context. The method can reflect their preferred way
of reviewing work.

### Assemble annotations without a model

**Input.** A folder of notes containing highlights and comments.

**Method.** Select annotations by existing marks, retain source order or sort
them by note name, and build a referenced index using direct transformations.
Use the existing annotation representations and collection logic.

**Output.** A collection tailored to the user's chosen material and organization.

**Experience.** The user gets a repeatable personal view of their notes. This
example also checks that the scripting operations are useful independently of
model generation.

## What this makes possible over time

Once the first methods work, the same foundation can support several extensions.
Each should be evaluated against an actual user need.

**Conversational invocation.** The user asks an assistant to apply a saved
method to a specified source. The assistant reads the skill instructions,
resolves missing inputs, and invokes its entry point. The script continues to
own any repeatable processing sequence.

**AI-assisted authoring.** The user describes a variation in ordinary language.
An assistant prepares changes to the skill's prompt or script notes for the
user to inspect and run. Successful experiments can become saved methods.
This requires explicit authoring operations and knowledge of CHA's script API.

**Reusing intermediate results.** A user can run another method on existing
L2 notes, a glossary, or a collection of claims. Ordinary note output provides
this composition naturally. Source references need to remain traceable through
the additional transformation.

**Additional media and sources.** Existing transcription or retrieval
capabilities could become operations when a concrete skill needs them. A
transcript can already serve as text input once it exists as a note. Audio
generation, direct retrieval, and external actions would each require their own
capability and product decisions.

## Proposed first delivery

The first delivery should provide skill resources in Notes, explicit invocation
on a note or folder, a single embedded scripting runtime, configured model
calls, structured source access, and output creation with usable references.
It should include the Book layers example and a small deterministic example.
Desktop Notes is the proposed initial authoring and execution surface.

Lua is the leading runtime candidate because embedding and host-supplied
functions are central to its design. The technical design should assess the
integration work, text and JSON handling, cancellation, and maintenance burden
before selecting it. The PRD requires editable execution logic and defined
host operations.
[Lua embedding](https://www.lua.org/pil/24.html)

Develop the first method alongside its required operations. Validate the
reading experience on one chapter, then a book. Expand the script API when a
second method reveals a concrete missing capability. Use the existing
background-job and provider infrastructure for execution.
[Background jobs](../src/app/background_jobs.h)

## Acceptance scenarios

| Scenario | Observable result |
| --- | --- |
| Edit a selection prompt and run again | The next run uses the saved edit without rebuilding CHA. |
| Change the script's sequence of generations | The next run follows the saved sequence and produces its expected outputs. |
| Save and reopen a script | Its source text, indentation, and line breaks are preserved. |
| Run the book method on one chapter | L2 excerpts match the source, L3 selections match L2, and L4 claims carry supporting references. |
| Follow an idea through the reading layers | Each reference reaches the relevant passage and returning restores the prior location. |
| Process a whole book | Chapter outputs appear as completed units and the entry note represents the completed work accurately. |
| Cancel during generation or local computation | Execution stops and already committed results remain usable. |
| Encounter an invalid model selection | The affected step reports a validation failure that identifies the relevant source. |
| Edit a source during processing | Affected results are checked against the source used and a conflict is reported before invalid references are published. |
| Run again after editing an earlier result | The default creates a separate result folder and preserves that edited result. |
| Run the deterministic example | It produces the expected collection through note operations alone. |
| Export and import a skill folder | Its resources remain editable and its internal references resolve. |
| Open generated references after synchronization or folder transfer | Targets resolve correctly when the required material is available, or an unavailable target is reported. |

User evaluation should answer whether the method is easy to start, whether the
generated levels help the user choose what to read, and whether changing a
prompt gives understandable control over the outcome. Runtime validation should
check reference correctness, partial failures, cancellation, and save behavior.

## Decisions for technical and interaction design

| Decision | Current direction |
| --- | --- |
| Authoritative storage | Ordinary Notes folders and documents, as established in the discussion. |
| Skill recognition | Choose a minimal convention for identifying a skill folder, instructions, and entry point. |
| Runtime | Evaluate Lua as the leading candidate. |
| Script editing | Reuse code blocks and determine the smallest editor change needed for convenient authoring. |
| Parameters and invocation | Define a small way to supply source, destination, and method options, then present the UI for approval. |
| Output behavior | A fresh result folder is the proposed default. Define explicit updating only for a method that needs it. |
| Source references | Resolve passage identity and navigation across editing, movement, synchronization, and import. |
| First reading surfaces | Validate desktop navigation first and decide when equivalent ChaWeb passage navigation is required. |
| Skill format compatibility | Decide whether the initial feature needs Agent Skills import/export or only a compatible conceptual structure. |
| Assistant integration | Add conversational invocation and authoring after direct scripted execution has demonstrated value. |

