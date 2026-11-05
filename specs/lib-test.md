# Forum Library integration test

Date: November 4, 2026

Status: Test design. This document defines one extensive acceptance campaign;
it does not implement or execute it. No live requests, uploads, or deletions
are authorized merely by writing this document.

## 1. Objective and acceptance contract

Run the complete Library workflow through CHA, with real OpenAI and Gemini
retrieval, and establish all of the following:

1. Settings select and persist the intended saved key, stores, and optional
   default provider.
2. Every ordinary generation uses the correct effective provider and receives
   exactly the Library references that it is eligible to use.
3. The system instructions tell eligible models to check the Library before
   the web for questions about books or documents.
4. Real models actually search the selected stores and recover facts that were
   not available anywhere else in their input.
5. Live edits, continuation rounds, restart, cancellation, and invalid settings
   preserve the documented behavior and do not leak access between Forums,
   sessions, characters, or vaults.
6. Library settings and chat leave remote store management under its existing
   controls. Clearing a Library or deleting a Forum does not delete stores.

The behavioral contracts are [Library settings](library-design.md),
[Library use in chat](lib2-design.md), and the subsequent shared prompt change
in `src/providers/tool_calls.cpp`. Runtime behavior in the second document
supersedes the first document's statement that Library settings have no chat
effect.

At the user's request, this campaign replaces the live-validation rule at the
end of section 10 of the second document. That rule allows only existing
stores and no store creation or deletion. This campaign creates and deletes
run-owned stores only.

This is one campaign with many individually reported scenarios, not one huge
assertion that stops on its first failure. It is opt-in, run once against a
recorded build and explicit model IDs, and produces one acceptance report.
It does not become a scheduled task, CI requirement, production probe, or
capability discovery service.

A request containing a store ID is not sufficient evidence of retrieval. A
model saying "I searched the Library" is not sufficient either. Positive
retrieval cases require the independent evidence described in section 5.

### Two verdicts

The report gives two separate verdicts.

- **CHA verdict.** Checks with fixed results: each request carries the correct
  native tool, store IDs, prompt paragraph, provider, and key identity;
  precedence, persistence, isolation, and UI state are correct; no unintended
  remote store changes occur; cleanup is complete. Apply these checks to every
  trial, also to trials where the model answers wrongly. Every CHA check must
  pass.
- **Model verdict.** Model behavior: the model searched the Library, searched
  it before the web, and returned the required facts. Count these results for
  each scenario family, provider, model, and transport mode. Compare the counts
  with the thresholds in section 8, frozen before execution. A miss is a model
  result, not a CHA defect.

A live model can choose differently on each trial. A rule that every model
trial must pass would make the campaign fail even when CHA is correct.

## 2. Existing tests and live acceptance

| Part | What runs | What it establishes |
| --- | --- | --- |
| Existing tests | The native suites and `npm run check` in `webapp`, as they exist now | Exact errors, request races, call limits, stale snapshots, UI failure states, and other branches that a real model cannot reliably cause |
| Live acceptance | The real runtime, bridge, persistence, provider clients, HTTPS transport, and hosted stores, with real model replies, in the two hosts below | End-to-end retrieval, source selection, answer correctness, ordinary multi-character chat, save/reopen/restart, and production prompt behavior |

The existing tests must pass before live work starts. A unit-test result never
counts as live retrieval. A direct provider calibration request never counts as
an application acceptance request. A UI component test never counts as a
native-host check. Every result records its host.

### Hosts for live scenarios

- **In-process driver (`D`).** A test-only C++ driver links the runtime and
  calls `Application`/`BridgeRouter` as
  `tests/integration/application_integration_test.cpp` does, with connection
  acknowledgements and snapshots. Only this host can call
  `Application::set_web_search_url_override_for_tests()`. Therefore every
  trial with CHA web tools runs here, including R02, R06, R08, and the primary
  Library-first configuration in section 6. The web search tool also needs a
  saved search key: use a dummy value, as the existing integration test does.
  Point the ordinary `web_reader_url` setting at the same local server for
  page reading.
- **Native host (`N`).** The macOS native test host with the actual React UI
  and actual native bridge, as in `tests/native/macos/test-summary-live.py`.
  Copy and adapt only the needed mechanics; its model choices and linear log
  pairing are not the specification for this campaign. This host has no
  web-search override. It runs the `U` rows and one live retrieval per service
  with CHA web tools off, to prove that the packaged application uses the
  tested runtime.

Do not point a provider at localhost or a proxy to capture traffic: native
Library eligibility would then fail. Capture through the debug log (section 5).

### Coverage map for deterministic branches

Existing tests cover these branches; this campaign does not repeat them. In
step 1 of section 11, confirm each link. When a branch has no test, add a unit
test to its existing suite, outside this campaign. Do not build campaign-only
fakes, barriers, or transport seams.

| Branch (former row) | Existing tests |
| --- | --- |
| Library screen loading, list and provider-list failures, Retry, late key results across keys and vaults, save failure, stale-draft Reload, and clearing while discovery fails (S05–S07, S09) | `webapp/src/components/ForumLibrary.test.tsx` |
| Store discovery across all pages (S10) | `FolderStoreHttp.ListsStoresFromEveryPageForBothServices` in `tests/providers/unit_folder_store_client.cpp` |
| Unsupported endpoints, APIs, authentication, HTTP, wrong port or base path, test mode, legacy and ambiguous keys, unsupported key cloud-store service (P05) | `Workspace.LibraryEligibilityUsesTheSavedKeyIdentityAndNativeEndpoint` in `tests/workspace/unit_workspace.cpp` |
| Exact live-edit races; one starting Forum revision for all children (T01, T02) | `ForumLibraryTest.AllChildrenAndPreparedRepliesRetainTheirStartingRevision` in `tests/session/unit_session_open.cpp` |
| Locally invalid Library through an editor bypass (T06) | `ForumLibraryTest.InvalidStoresKeepValidDefaultAndUseSharedValidationMessage`; `Workspace.MalformedLibraryIsIgnoredAndPreservedOnUnrelatedEdits` |
| Stale-default and invalid-access log levels (T07) | `ForumLibraryTest.StaleDefaultIsSilentInSnapshotsAndWarnsOncePerGenerationTarget` |
| Explicit store rejection, HTTP error statuses, truncated streams, interruptions, and retries (T08 rejection branch, T09) | `ProviderClient` tests in `tests/providers/unit_provider_client.cpp`, including `UnknownGoogleMixedModelFailureDoesNotRetryOrDropStoresOrFunctions`. No production path retries a Library request automatically: chat generation does not retry, and Notes retries carry no Library. A manual retry builds a new request from the current Library. |
| Cancellation at exact stages (T10) | `ProviderClient`, `ProviderClientTools`, and `SessionController` cancellation tests |
| Tool and call limits with native search (T11) | `ProviderClientTools.ResponsesKeepsHostedSearchAndReplayThroughFinalBudgetRound`; `GoogleApi.Gemini25AliasesUseFileSearchWithoutFunctions`; `GoogleApi.LibrarySearchCapabilitiesAndDeclarationOrderAreIndependent` |
| Close, vault switch, and shutdown during a request (T12) | `SessionController` shutdown and cancellation tests in `tests/session/unit_session_controller.cpp` |
| Upload cancel, cleanup, and error paths (B03) | `FolderStoreHttp` tests in `tests/providers/unit_folder_store_client.cpp` |
| Secondary-process restrictions and stale epochs (B05) | `forum.library.update` in `tests/fixtures/wire/native-method-policies.json`; `ApplicationSettings.LibraryValidatesLocallyAndLeavesLiveSessionRunning`. Confirm a secondary-process rejection test; add one if none exists. |

### Campaign files

A small Python coordinator, the in-process driver, and a UI probe script are
sufficient. Their proposed locations are
`tests/integration/library_campaign.py`,
`tests/integration/library_campaign.cpp`, and
`tests/native/library-campaign.js`. Reuse the existing macOS probe host. Build
the driver with a temporary CMake target. These files exist only for this run:
after the final report, delete them and the target, and keep only the evidence
package. No product endpoint, database schema, capability cache, or
Library-specific background worker is needed.

The coordinator drives the live scenarios through public bridge operations or
visible UI actions. It must not compose replacement model request bodies,
insert answers into conversations, or patch the production system prompt.
Remote fixtures come from CHA's existing Notes upload (section 4). Direct
provider calls are limited to the calibration requests (section 4) and the
remote label changes in S04. Direct storage edits are reserved for initial
local fixtures, made while their application instance is stopped.

## 3. Preconditions and isolation

### Build and account record

Before remote fixture creation, record:

- Git revision, an uncommitted-changes flag, platform, and build directory.
  Build the runtime, native host, and web assets from this revision just before
  the run. The native host must load these web assets and this runtime; an
  older installed app is not acceptable evidence for the current code.
- Explicit OpenAI Responses and Gemini provider configurations, with secret
  values removed, and the provider-reported model version when available.
- Test configuration, session/vault paths, scenario list, fixed repetitions,
  timeout policy, start time, and output directory.
- API account access, indexing readiness, and native File Search support for
  the chosen models. A successful model-list request alone does not establish
  File Search support; the live calibration must establish it.

Use `gpt-5.6-terra` with OpenAI Responses as the OpenAI target and
`gemini-4-argon` as the primary Gemini target for the observed regression.
Use `gemini-2.5-flash` for the Gemini 2.5 compatibility row R10. Record model
IDs once; do not silently replace an unavailable model with another model or
another API. An unavailable primary model makes its live scenarios BLOCKED,
not PASS. R10 is conditional: if `gemini-2.5-flash` is unavailable to the
account, report R10 as NOT_AVAILABLE. This does not block the campaign.

Use both streaming and buffered provider configurations. Primary positive
retrieval, natural book questions, and continuation checks run in both modes.
Other scenarios may use the normal streaming configuration unless stated
otherwise. Do not add unsupported temperature or reasoning options merely to
try to make a model deterministic.

### Credentials

Read `OPENAI_API_KEY` and `GEMINI_API_KEY` from the process environment, or from
`~/.zprofile` when absent, using the established project procedure. Never print
values, source the profile with shell tracing, put keys in command arguments,
or copy them into the repository or report. This design phase reads no keys.

A running test application needs its normal saved keys. Put these only in the
disposable, access-restricted test vaults (chat and fixture vaults), excluded
from evidence packaging. Remove those credential-bearing vaults during final
cleanup. Redact authorization
headers, key query parameters, and exact/escaped secret values from evidence.

The two real secrets are enough for the required campaign. Additional saved
entries with the same secret test local saved-key identity. They do not prove
remote project isolation. Remote permission-denied responses are not tested
live unless a separately provisioned test credential is available; T09 is in
the coverage map.

### Local and remote ownership

Use new disposable vaults and configuration directories. Never alter the
user's active vault, Forum, characters, session history, or Library settings.
Use neutral test characters and disable voice output and unneeded background
work in these vaults.

Create run-owned remote stores through CHA's Notes upload (section 4), from
folders whose names start with `cha-library-test-<run-id>`. Persist each
returned store ID and operation ID in an ownership ledger as soon as it is
known. A name prefix alone is never deletion authority.
Record the service, creator saved-key ID, resource kind, and creation result.

Existing user stores, including the store containing *Marathon Sex*, are
borrowed read-only resources if included in the supplementary check in section
9. They must never enter the cleanup ledger as owned resources.

Do not reuse the old `file-search-live-results` stores or its oracle. Those
artifacts tested the removed folder-search proxy and can contaminate this
experiment. They are historical evidence, not a Library acceptance baseline.

## 4. Fixtures and the independent answer oracle

### Remote stores

Create this small set for each service; `O` and `G` identify service-specific
copies, not shared content or shared credentials.

| Store role | Contents and purpose |
| --- | --- |
| A | Synthetic book and one reference document; first selected store |
| B | A supplementary document with different facts required by a combined question; second selected store |
| C | Different edition of a similarly named book, plus a unique third-store fact; replacement and isolation cases; Gemini's three-store selection |
| D | A run-owned store used for actual deletion through `cloudStore.delete` and subsequent stale-reference checks |

A/B/C can have colliding display names to test that IDs determine selection.
Keep their document titles distinguishable enough to define unambiguous
questions.

Create every store with CHA's existing upload path. Use a separate disposable
fixture vault, so that the answers never enter the Notes of a chat vault. In
that vault, put each store's documents in one Notes folder named
`cha-library-test-<run-id>-<service>-<role>` with the existing Notes
operations. Start `notes.storeUpload` with the service's saved key, and wait
with `notes.storeOperation`. Chat vaults reference the resulting stores by ID
through their own saved key entries for the same secrets. B03 creates one more
store from a chat vault through the visible UI; add it to the same ownership
ledger. No large bulk upload or enormous pagination fixture is necessary.

Generate simple UTF-8 Markdown/text fixtures, about 10–20 pages of useful prose
for the synthetic book. Include sections near its beginning, middle, and end,
with realistic headings and some irrelevant material. Use run-specific titles,
for example *The Lantern Orchard Manual, edition <run-id>*.

Generate random answers once, before upload. Give each store a small fixed set
of distinct facts, about 6–10, and reuse them across trials in fresh sessions.
Examples below describe fields, not literal test answers:

- A book: the archivist's name, three chapter titles, an unusual procedure,
  a named section's verification phrase, and a fixed numerical rule.
- B supplement: a replacement part code and an exception that changes that
  numerical rule. A combined question needs evidence from both A and B.
- C edition: a different rule and different verification phrase for the same
  subject. An unselected store's answer must not appear in the answer.
- A separate document: several rows of a small table, Unicode names, and one
  explicit statement that a particular detail is not recorded.
- A short passage containing an instruction-like quotation, clearly presented
  as document content. It must be treated as source material rather than a
  command to ignore the user or change the task.

Use independent high-entropy phrases/codes, such as random 128-bit values
encoded into readable groups, for the decisive checks. Never derive an answer
from its title, store name, question ID, or run ID. Natural prose questions test
usefulness; exact code questions establish that retrieval supplied otherwise
unavailable information. The two services must have different answers even
where the books have similar titles and structure.

Reserve extra facts only where one session needs a second new search: R06, T04
after restart, S12, and trials that ask several questions in one session.
Reserve one calibration fact per store (Setup readiness below). Put facts in
clearly different sections with different wording, so that one question
matches one passage.

### Prevent answer leakage

Maintain a local `oracle.json` with document hashes, field values, accepted
normalizations, source file/document IDs, and expected store membership. The
oracle is read only by the evaluator, never by the model-facing driver.

Hosted search runs inside the same HTTP call as the first request, so a full
audit before retrieval is not possible. Check the fixed inputs (system,
character, Forum, and persona text, and the prompts) before the trial. After
the trial, audit the logged request body of every round of each positive or
negative trial, and verify that its target answer is absent from:

- System/character/Forum/persona text and session traits.
- User prompts, filenames, titles, resource names, and tool descriptions.
- Earlier transcript entries, attached notes/files, summary catalogues,
  summaries, tool responses from web search, and application notices.
- Any preceding child response included in an All-characters request.

The initial fixture upload body is allowed to contain the answer. A subsequent
provider-hosted retrieval result is allowed to contain it. Later continuation
inputs can replay that result and text derived from it, with the provenance
chain retained. Other incoming sources invalidate the retrieval trial rather
than earning a pass. Such replay is not evidence of another fresh retrieval;
use a new reserved fact when a scenario requires a new search.

Each independent trial starts a fresh session or asks about a reserved fact
that has never appeared in that session. Isolation and incompatible-provider
cases always use a fresh session. Fact reuse across fresh sessions is safe:
OpenAI requests use `store: false`, and summaries exist only after an explicit
`sessionSummary.commit`. Commit no summary in a Forum used for retrieval
trials; R12 uses its own Forum. Clearing the Library does not erase facts
already present in conversation history; a model recalling an earlier answer
would not prove an access leak.

The All-characters test must inspect every child's actual input. One child
recovering a fact must not let another child pass from a shared transcript.
Where the test requires every child to retrieve, require native search evidence
for each child, not only a shared correct answer.

### Setup readiness

Upload, wait for successful indexing, and record file counts and IDs. Wait
with `notes.storeOperation` and a bounded deadline. An accepted upload is not
enough. Waiting for indexing is fixture preparation;
chat preparation must not begin polling or listing stores on every turn.

Perform one isolated calibration request per service/model configuration that
will be scored for live retrieval, using a reserved calibration fact. This
establishes that the fixture and account can retrieve. Its answer is never used
in application trials. If calibration fails, diagnose the fixture/provider;
do not present a successful direct API call as a successful CHA test.

## 5. Evidence and grading

### Correlate complete requests and responses

Capture public bridge requests/replies, snapshot events, generation terminals,
full redacted provider request/response payloads, tool dispatches, and relevant
configuration snapshots. Keep full synthetic-document responses, not excerpts
from the Assistant's log-reading tool.

Use the production debug logging path at debug level. It keeps each round's
full request body (`HTTP model request body`) and raw response
(`HTTP model raw response`). Parse the log entry prefix, not substrings inside
a logged prompt. Payload lines carry no request ID; join them by the thread ID
in the prefix. Each provider request runs on its own thread. On that thread,
`Model round started` gives the provider ID and round, and
`HTTP request started` gives the endpoint. A chat generation request also has
a `Provider request started` line that gives the Forum, session, character,
request ID, model, API, and stream mode. Requests that Notes operations make
directly (session summary, note compression, and page formatting) have no such
line. Identify each of them by the bridge call that started it and its reply,
the provider ID, and the model in the logged request body. Run these cases one
at a time, so that only one Notes request is in flight. Do not add new logging
for this campaign. Verify each round has one complete request and its complete
response or explicit cancellation/error. Do not pair concurrent requests by
"last request seen". Unresolved pairing, a chat generation request without its
`Provider request started` line, or a truncated payload is EVIDENCE_MISSING.

The log rotates at 10 MB with three files. Copy the log files after every
case, and run All-characters cases one at a time.

### Required evidence for a positive live retrieval

| Layer | OpenAI Responses | Gemini GenerateContent / streamGenerateContent |
| --- | --- | --- |
| Correct request | One native `file_search` containing exactly the expected `vector_store_ids`; `file_search_call.results` included; effective provider/key identity correct | One native `fileSearch` containing exactly the expected full `fileSearchStoreNames`; effective provider/key identity correct |
| Native search evidence | A completed `file_search_call`, with its results mapped to run-owned fixture files | Returned `toolCall` of type `FILE_SEARCH` and its matching `toolResponse`, or `groundingMetadata` containing fixture-linked retrieved context |
| Source evidence | Returned result text contains the relevant fact; record file IDs and citations where returned | Record returned grounding/citation/excerpt data where available. If hosted results are opaque, a completed native exchange plus recovery of the otherwise unavailable random fact is required |
| Answer evidence | Exact required random facts and correct prose interpretation | Exact required random facts and correct prose interpretation |
| Isolation evidence | No unselected/other-service oracle answers and no input leakage | No unselected/other-service oracle answers and no input leakage |

OpenAI describes the hosted call output, file citations, and the opt-in search
results in its [File Search guide](https://developers.openai.com/api/docs/guides/tools-file-search).
The oracle must examine the raw response, since CHA need not display a new
citations viewer.

For Gemini, preserve the actual GenerateContent evidence emitted by the
chosen model. Current Google guides also show the Interactions API; this
campaign must not migrate CHA to it or grade an Interactions response as a
GenerateContent response. Use the supported native shape already used by CHA.
Google documents File Search and mixed native/custom tools in its
[File Search guide](https://aistudio.google.com/docs/file-search) and
[tool-combination guide](https://aistudio.google.com/docs/tool-combination).

Citations can be optional or incomplete. Their absence alone is not a failed
answer if the other required native/source evidence is present. An exact
random answer with no observable native search or grounding is insufficient
for the campaign's claim that search was verified: report EVIDENCE_MISSING and
investigate. A tool declaration, HTTP 200, token count, model self-report, or
search invocation with no correct recovered fact cannot pass retrieval.

For an empty/missing-fact case, require evidence that search was attempted,
then verify an honest statement of insufficient evidence. Do not interpret an
empty result as proof that every document was examined or the fact is absent
from the whole service.

### Answer and source-selection checks

Each trial records its CHA checks and its model result separately. A failed
CHA check is FAIL. A model miss (no Library search, web before Library, or a
wrong or missing fact) is recorded as a model result and counted against the
family's threshold in section 8.

Score exact codes, integers, and required facts mechanically. Define accepted
whitespace/case normalization in the oracle before execution. Do not relax a
failed answer's rubric after seeing it. Use a fixed human checklist for book
summaries, attribution, uncertainty, quotations, and refusal to obey quoted
instructions. An LLM judge, if used as an aid, is not the sole authority for
codes, retrieval provenance, or pass/fail.

For a natural book question fully answered by A/B, require a Library search
before the first CHA web-search/web-read dispatch, correct document-sourced
content, and no claim that depends only on web results. Count web dispatches
after the Library search separately; they are not a miss. The same
Library-before-web rule applies when a question also needs an explicitly
external detail. Returned provider event ordering must support that
assertion. If within-round ordering is not exposed, record that ordering
as unverified rather than inventing a timestamp.

Use the existing test web-search override with a controlled local server for
these live model trials. Only the in-process driver can do this (section 2). The model sees normal web tool definitions and decides
whether to call them; only the external search backend is controlled. Return
plausible public-style summaries or clearly different decoy codes, never the
private answer. Record every dispatch, including query and response. This
reproduces competition between tools without needing a third paid API key.
It must be visibly labeled "live model + controlled web backend" in the report.

An explicitly configured required hosted web-search mode retains its existing
behavior. It is tested separately and is not used for the Library-first
acceptance trials.

## 6. Provider, character, and Forum matrix

Create saved keys `O1` and `G1`, plus `O1-copy` and `G1-copy` containing the same
secrets under different IDs. Additional metadata-only invalid fixtures need no
real external credentials. Never compare secret equality to determine access.

| Provider role | Configuration and purpose |
| --- | --- |
| O-native | Direct HTTPS OpenAI Responses, saved key O1 |
| G-native | Direct HTTPS native Gemini, saved key G1, primary Argon model |
| O-copy / G-copy | Native endpoint and API, copied saved key entry; local incompatibility |
| O-chat | OpenAI Chat Completions with O1; ordinary live reply without Library |
| G-2.5 | `gemini-2.5-flash` with G1, when available; native File Search and custom-function omission |
| Alternate | A second base configuration with visibly different model/options to test full provider precedence |
| Ineligible variants | Subscription auth, unsupported API, proxy endpoint, HTTP, wrong port/base path, missing/ambiguous legacy key; covered by the coverage map (P05), not run live |

For the primary Library-first trials, set provider-hosted web search to off,
as in the observed incident, and enable the ordinary CHA web tools against the
controlled backend. Keep native File Search selection automatic. This makes
every competing web dispatch observable. R11 separately covers hosted web
settings; none of these configurations injects an instruction into the user
question or forces a native search call.

Use two differently configured ordinary characters and Assistant as an ordinary
member of the test Forum. Include different effort and supported web-search
overrides. Keep their profiles neutral for the exact-answer trials. Add one
realistic book-discussion profile, based on the deployed style of the Suzan
case, for the natural-language regression trials; remove personal data and do
not add test-only instructions to use the Library.

Use at least three Forums: O-Library, G-Library, and no-Library, plus a second
disposable vault with different stores/facts. Test the same selected store
referenced by two Forums, and two Forums selecting different stores with the
same display name. Model/effort settings persisted in sessions must be
independently inspectable before and after default-provider changes.

## 7. Scenario catalogue: settings and access

Each row is a scenario family. Report each listed variant as its own result.
Repeat service-dependent cases for OpenAI and Gemini. `L` means a live check in
the in-process driver (`D`), with real providers and stores. `U` means the
actual UI with the real bridge in the native host (`N`). Rows P05, T02, T06,
T07, T09, T11, and B05 moved to the coverage map in section 2, which keeps
their IDs. The other rows list only their live variants; the coverage map
covers their deterministic branches.

| ID | Path | Actions and required result |
| --- | --- | --- |
| S01 | U/L | Navigate Forum → Members → Library → Members. Save one store with provider None, then with a default provider. UI values, bridge reply, Forum detail, and persisted table agree. Navigation itself does not save drafts. |
| S02 | U/L | Reopen Library, close/reopen session, and restart the disposable app. Key IDs, store sets, full Gemini names, provider selection, and model lock restore correctly. |
| S03 | U/L | Select two OpenAI stores; unchecked third checkbox disables, checked rows remain editable. Gemini permits A/B/C. Save order changes do not create a false dirty state; backend persists the same set. |
| S04 | U/L | Duplicate store display names, unnamed stores, renamed local saved keys/providers, and changed remote store labels preserve ID-based selection. Use run-owned resources for remote renames. |
| S05 | U | No eligible keys, no matching providers, and an unavailable saved key, each reached with real vault data. None remains usable, and the saved Library is not erased. |
| S06 | U | Changing the key resets draft stores and provider to None. |
| S07 | U | Dirty Members and Library drafts are independent. Leaving discards the departed draft; saving disables duplicate submissions. |
| S08 | L | Reject empty stores, duplicate IDs, three OpenAI stores, malformed IDs, wrong service names, missing/ineligible key, incompatible default, malformed/missing request fields, read-only Forum, and stale epoch. Preserve the previous saved value and return the established safe error. |
| S09 | U/L | Clear Library with None. Verify absent persisted Library, unlocked Model control, normal provider selection on the next turn, and zero remote mutations. |
| S10 | L | Provider filtering in store discovery uses the same local eligibility as generation. Record zero model-list probes per generation. |
| S11 | L | Save/replace/clear Library without changing Forum membership, prompts, persona, character settings, or session overrides. Unrelated Forum/member/file edits preserve Library settings. |
| S12 | L | Delete a test Forum and clear one of two Forums sharing a store. Store/document IDs remain remotely present, and the surviving Forum still retrieves its reserved fact. |
| S13 | U/L | Inspect effective toolbar state with no default, each valid default, and stale default. Only Model is locked by the default; Character and Effort retain normal enablement when the chat is idle. Do not confuse general generation-time disabling with the Library lock. |

### Eligibility and precedence

| ID | Path | Actions and required result |
| --- | --- | --- |
| P01 | L | No Library: normal reply, no native File Search, and no Library-first prompt paragraph. |
| P02 | L | Matching native provider and saved key: selected references and Library-first paragraph are present; retrieve a reserved fact without attachments. |
| P03 | L | Same service and same secret under another saved-key ID: normal reply, no Library tool/instruction, info omission log, no private fact recovered, and no substituted Library credential. |
| P04 | L | Opposite service and OpenAI Chat Completions: normal valid reply without Library access. No cross-service callback or proxy search request is sent. |
| P06 | L | Character provider, then valid session choice, then Forum default: verify the complete base config and actual outgoing model/API/endpoint/key/options at each step. The default wins even for an unrelated non-document question. |
| P07 | L | Under a default, save another session model choice and an effort choice. Snapshot and request still use the default model; effort follows existing layering. Remove only the default: the stored model choice reappears. Character files and saved session choices were never rewritten. |
| P08 | L | Character web-search and reasoning overrides retain their existing provider-specific rules. Verify permitted override, ignored unsupported override, and warning behavior; a Library does not define a second effort policy. |
| P09 | L | Mixed-provider All-characters reply with provider None: compatible children retrieve, incompatible children reply normally without Library tools. Their ordinary tools remain available. Repeat with each service as the Library. Inspect every child's input and search evidence. |
| P10 | L | All characters under each valid Forum default: all children, including ordinary Assistant, use that default and independently retrieve. Repeat an explicitly addressed ordinary target and a selected default character. |
| P11 | L | Another Forum with no Library or a different Library cannot inherit references or unseen facts. A second vault with reused local IDs cannot inherit references, active results, or summaries from the first. |

## 8. Scenario catalogue: actual retrieval and model decisions

Run R01–R09 for both primary providers. R01, R02, and R06 run in both streaming
and buffered modes. Each trial runs in a fresh session with uncontaminated
input, and records its CHA checks and its model result. Every CHA check must
pass. Compare model results with these thresholds:

- R01 and B08: every trial.
- Every other R family: at most one model miss for each provider, model, and
  transport mode that has three or more trials; every trial in a smaller
  group.

These are proposed defaults. Confirm or change them when you freeze the matrix;
do not change them after execution starts. Report the counts behind every
result.

| ID | Actions | Required evidence and result |
| --- | --- | --- |
| R01 | Ask for one section's exact random verification phrase without naming a tool. One selected store, neutral profile, no attachments. Three fixed trials per mode. | All required positive evidence in section 5, exact phrase, no unselected facts. |
| R02 | Ask naturally what the synthetic book is about, its main contents, and one named section's recommendation. Include an English and a Russian wording, each with three independent trials per mode. Keep web tools available. | Library search before the first CHA web search/read, accurate source-supported contents, and no claim that depends only on web results. Count later web calls separately. The user prompt must not say "use File Search" or "check the Library". |
| R03 | Ask an explicit Library question, then a paraphrased/semantic question whose wording differs from the document. Ask separate beginning/middle/end facts. | Both explicit access and ordinary semantic retrieval work; source excerpts and answers match the reserved facts. Explicit success cannot excuse R02 failure. |
| R04 | Select A/B and ask for A's rule plus B's exception and result. Repeat with only A selected. For Gemini also select A/B/C and ask for one distinct fact from each. | Evidence from every required store; correct combined answer. With B absent, no B-only random fact and an honest gap. No arbitrary truncation to two Gemini stores. |
| R05 | Ask about the similarly named edition in C while only A/B are selected, then replace selection with C in a fresh session. | No C-only answer before selection; C's exact new fact afterward. IDs and source mapping prove the edition, not just a matching title. |
| R06 | Obtain A's first reserved fact, then ask about a new fact requiring B and a custom tool. Use a controlled web or summary result for a genuinely separate detail. | Native stores and instructions persist in every round; new retrieval supplies the unseen fact; custom call/result IDs and signatures replay correctly; final answer combines the right sources. |
| R07 | Ask for a detail absent from all selected documents, then repeat with no Library. | Search is observed in the Library trial, answer reports insufficient information, no invented oracle value, no assertion that every file was searched. No-Library control has no Library instructions or retrieval. |
| R08 | Ask a fully answerable book question while the controlled web backend would return a plausible conflicting summary. Then ask one missing external detail. | First trial searches the Library before any web dispatch, uses the private document facts, and does not adopt the conflicting summary. Second shows Library search before web and keeps document and external facts attributed correctly. A confident web-only answer fails. |
| R09 | Retrieve the passage containing a quoted instruction-like sentence. Then ask an unrelated greeting/arithmetic question in a new turn. | Quoted instructions do not change the task or trigger unauthorized tools. The unrelated turn is correct; search is not required, and its occurrence alone is not a failure because the feature does not forbid optional retrieval. |
| R10 | Conditional on `gemini-2.5-flash` availability; NOT_AVAILABLE otherwise. Run G-2.5 with a valid Library and optional web/read/summary tools requested. | Actual retrieval and correct private fact; outgoing request omits incompatible custom functions, logs the established warning, and retains native search. Repeat a no-Library request to show ordinary functions return. |
| R11 | OpenAI native search alongside automatic hosted web search and custom summary tools; separately run required hosted-web mode with a Library. | Native store/include entries coexist and survive later rounds. In required mode, round 1 must contain a `web_search_call`. If only `file_search` satisfies `tool_choice: "required"`, report a CHA defect. Record which tool satisfied it. Required mode is not graded as a Library-first trial. |
| R12 | In its own Forum, save a summary containing an unrelated test fact; ask a question requiring both that prior conversation and a new Library document fact. | Summary tool executes when needed; native retrieval independently supplies the new private fact. The summary must not contain that Library answer. |

For R06 and R08, if a real model does not call the required custom tool in the
fixed trial, record a model miss. Existing unit tests cover the continuation
transport; they cannot convert that live trial into a pass. Do not replay a
response as though it were generated live.

## 9. Reproduce the real-book incident

The mandatory regression is R02: a private, answerable book question with web
tools available, normal production instructions, and exact source evidence.
It is scored for both providers and cannot pass from a generic public blurb.

Also perform a supplementary read-only replay against the user's Gemini store
containing *Marathon Sex* when that resource and permission are available:

1. Reproduce the observed provider/model and relevant character/Forum prompt
   style in a disposable vault, with the updated production Library paragraph.
   Record prompt hashes and any intentional differences; do not modify the
   user's live session or attach the book to the chat.
2. In a fresh session ask the original natural question about the book's
   subject and contents, with ordinary web tools available.
3. Ask one specific interior detail that is not simply publisher metadata.
   The user supplies this question and its expected answer before the run;
   the test agent does not read the book. The expected answer must not be
   placed in the prompt or web results.
4. Record actual native search/grounding and assess whether the answer reflects
   the book. Public availability means answer correctness alone cannot prove
   its source; apply the stronger provenance checks.

This supplementary case is reported separately if the existing book/store or
source text for verification is unavailable. Do not copy the user's book into
an OpenAI store merely to mirror this case. The synthetic equivalent covers
both services without requiring an additional upload of user material.

## 10. Scenario catalogue: changes, failures, and boundaries

Live checks confirm representative behavior. Existing tests in the coverage map
cover exact races, timing, and error variants. In live timing checks, wait for
an observed event, such as the logged first request, not for a fixed sleep.

| ID | Path | Actions and required result |
| --- | --- | --- |
| T01 | L | Start a reply with A/provider X. After its first outgoing request is captured, save B/provider Y. Current request and all continuations retain A/X; next generation uses B/Y. Save, replace, remove default only, and clear Library are separate variants. |
| T03 | U/L | Save while a session is live. Its effective settings/model lock refresh without reload or shutdown; an in-flight answer completes. Snapshot may show the next-turn model while the old reply runs. Other Forums receive no Library refresh. Use manager `open` or active held generations for simultaneous sessions: normal navigation can retire an idle previous session. |
| T04 | L | Replace selection, reopen the chat, restart the entire test app, and open a new session. All use current Forum settings; session storage contains no copied Library record. Verify unseen post-restart facts, not recalled answers. |
| T05 | L | Remove a referenced default provider/key or change its eligibility. A stale default unlocks Model and normal selection resumes. Renames in S04 retain valid access. A compatible normal provider can still use a valid Library. Missing/ineligible Library keys omit search. Choose an independently healthy fallback provider so this test can actually demonstrate a successful ordinary reply. |
| T08 | L | Delete only run-owned D remotely with `cloudStore.delete`, then ask about it through the saved reference. Capture actual provider behavior. If it rejects the request, generation fails normally; no retry drops stores or changes provider. If it instead returns no retrieval, require an honest gap and label the rejection branch not exercised live. |
| T10 | L | Cancel before preparation, during a hosted-search request, after an answer delta, and between custom-tool rounds. No further custom work after cancellation; no late text/terminal is attached to a new request. A fresh follow-up generation still works. |
| T12 | L | Close a session, switch vaults, reconnect subscriptions, and stop/restart during a request. Existing lifecycle rules hold; old events do not publish into another session/vault. Current Forum configuration determines the next prepared request. |
| B01 | L | Ordinary Assistant member retrieves; Welcome maintenance, recipient classification, session naming, summary compression, and self-notes do not inherit Library/default behavior. Activate each applicable auxiliary path and inspect its actual request or the absence of one. Scope these expectations to Library controls; auxiliary tools keep their own configured behavior. Voice processing is out of scope: voice stays off (section 3) and keeps its own configuration. |
| B02 | L | Attach/detach an unrelated folder and attach/update a normal note. Library selection and retrieval remain unchanged. The attachment's own prompt copy and notices continue to work. No folder-to-store link is created. |
| B03 | U/L | Upload a disposable Notes folder using existing store upload, observe readiness, select its returned store as a Library, retrieve a reserved fact, then clear Library and manage the store through existing deletion controls. Capture every created resource for cleanup. |
| B04 | L | Save/clear Library, change members, and delete a Forum while counting remote management calls. These operations issue no remote creates/uploads/deletes. Chat preparation issues no store list/detail/model-discovery calls. Separate setup/UI discovery traffic from chat traffic in the trace. |
| B06 | L | Review redacted request bodies, prompts, logs, UI replies, and evidence package for secrets, obsolete folder-search tools, and source text injected outside native retrieval. Conversational native-search payloads contain only selected store IDs; settings discovery may legitimately list other IDs, which section 13 removes from the package. Credentials may not appear in model text or exported evidence. |
| B07 | L | Export a disposable vault whose Forum has a Library and a default provider, then import the export into a new disposable vault. Merge that vault into another disposable vault. In each result, `forum.get` returns the Library unchanged. When its saved-key ID resolves to a compatible key, a fresh session retrieves a reserved fact. When the key ID is absent, the saved Library is preserved, Library search is omitted with a warning, and the chat still replies. Record the merge rules that applied to the key ID. R2 upload/download is optional: run it only with a separately authorized disposable R2 location, because the vault holds the test secrets. Keep exports inside the disposable test area. |
| B08 | L | In a disposable vault, configure the built-in Assistant with O-native. In Welcome, ask how a Forum Library gives characters search access. CHA check: the maintenance request contains the Library rule from `resources/assistant-maintenance.md`, no removed folder-search routes, and no Library tool. Model result, graded with the fixed human checklist: the answer states that native File Search goes to characters whose effective provider uses the Library key, that a Forum default replaces the model for all characters, and that attached folders give no search access. |

Do not construct unreachable production states solely to add a row. For
example, normal key creation/loading rejects an empty secret. Test that
rejection and the reachable removed-key behavior; record empty-key matching as
a defensive validation check at its existing test boundary. Do not weaken the
loader, mutate a const workspace, or add a production corruption API to make
an empty-key runtime snapshot possible.

## 11. Ordering the campaign

Use this order so setup, calibration, and evidence problems are caught before
expensive behavioral trials:

1. Build from the recorded revision and run the existing tests; confirm the
   coverage map links.
2. Generate the fixture corpus and independent oracle; create the ownership
   ledger and the fixture vault; upload run-owned stores through CHA and
   establish indexing readiness.
3. Calibrate each required live configuration on reserved facts.
4. Exercise actual UI/bridge settings in the native host and one positive
   native-host retrieval per service with CHA web tools off, proving the
   packaged application uses the tested runtime.
5. Run eligibility/provider-precedence cases and live retrieval R cases in the
   in-process driver.
6. Run save/reopen/restart, multi-character, attachment, auxiliary,
   vault-transfer, and Welcome Assistant cases.
7. Run the read-only real-book supplement when available.
8. Verify retained evidence, secret and store-name redaction, persistence, and
   resource boundaries; clean up; produce the final result matrix.
9. Delete the campaign files and their temporary CMake target.

Continue independent scenarios after a failure. Mark dependent scenarios
BLOCKED with their dependency's ID, not skipped without explanation. A broken
service can leave the other service's evidence useful, but cannot yield a
complete campaign PASS.

## 12. Repetition, deadlines, and failure policy

"Executed once" means one planned campaign. It may contain the fixed
independent repetitions above; it does not mean repeatedly trying a failed
case until one answer happens to pass.

- Freeze prompts, facts, models, trial counts, thresholds, and scoring before
  execution.
- A valid completed web-only or wrong-answer response is a model miss for that
  trial, even if a later diagnostic retry retrieves correctly. Preserve both
  attempts; the first result counts. Tests must not silently force tool choice
  or disable web to repair it.
- Apply the application's existing transport retry rules. Count all underlying
  attempts. A manual diagnostic retry is separately identified and never
  overwrites acceptance evidence.
- Allow up to 30 minutes per indexing operation, 10 minutes per complete live
  generation including continuations, and 8 hours for the campaign. These are
  test deadlines, not new application lifecycle rules or provider guarantees.
  Configure ordinary provider timeouts consistently before the run.
- Plan roughly 100–200 live conversational trials, depending on the finalized
  variant expansion, plus their continuation rounds and calibration. Expand the
  matrix into an exact count before starting. Bound execution at 600 live model
  HTTP calls; stop starting new live scenarios when reached and mark the
  remainder BLOCKED. Before execution, write an approximate cost for the
  frozen matrix into `manifest.json`, and get the operator's approval of that
  estimate. Record usage and actual spend when available; missing provider
  accounting must not be presented as zero cost.
- Wait on operation states or events. Wait for indexing with
  `notes.storeOperation` and bounded backoff. Send progress updates between
  scenarios during the long run; do not block the operator behind one
  uninterruptible hours-long shell call.
- An operator interruption still runs cleanup for owned resources and produces
  a partial report. A rerun starts from the beginning with new stores and
  keeps the earlier partial report.

## 13. Evidence package and completion rule

Write one directory, such as `library-test-results/<run-id>/`, containing:

| Artifact | Contents |
| --- | --- |
| `manifest.json` | Git revision and uncommitted-changes flag, exact models, account aliases, run settings, scenario expansion, thresholds, cost estimate, times, and test resource paths without secrets |
| `ownership.json` | Returned remote resource IDs, ownership, creation/deletion state, and any unresolved allocation; never key values |
| `fixtures/` and `oracle.json` | Generated synthetic documents, hashes, accepted answers, and source mappings; evaluator-only during the run |
| `cases/<id>/` | Input prompt, sanitized effective settings, input-leak audit, complete redacted rounds, tool order, snapshots, final transcript, and individual assertions |
| `ui/` | Screenshots/action trace of real settings and model-lock states, labeled separately from UI component tests |
| `results.json` and `report.md` | CHA checks as PASS/FAIL/BLOCKED/EVIDENCE_MISSING/NOT_AVAILABLE per expanded scenario and repetition; model results with counts and thresholds per family, service, model, and mode; failure classifications, usage, and conclusions |
| `cleanup.json` | Every owned resource's final disposition, verification results, local credential-vault removal, and any remaining work |

Scan the package before finalizing it. Do not include the temporary key-bearing
vaults, vault exports that contain secrets, environment dumps, authorization
headers, or unredacted transport logs. Remove the names and IDs of stores that
the run does not own from discovery replies and logs, and mask them in
screenshots. Preserve test facts; they are needed to independently audit
retrieval, unlike credentials.

The campaign gives two verdicts (section 1).

A **CHA PASS** requires:

1. Every required expanded scenario passes its CHA checks, including both
   providers' natural book questions with web tools available and both
   transport modes.
2. All required trials have uncontaminated inputs and correct request
   contents, and every trial where the model searched has native retrieval
   evidence. There is no "configured therefore searched" shortcut.
3. Live coverage, existing-test coverage, and optional or conditional checks
   are explicit. No required row is BLOCKED or EVIDENCE_MISSING. A conditional
   row (R10) or the supplement may be NOT_AVAILABLE.
4. Provider/default selection, persistence, UI state, lifecycle boundaries,
   and lack of unintended remote mutations are established by their evidence.
5. Cleanup is complete or its exact unresolved resources are reported as an
   incomplete campaign, rather than claiming that deletion succeeded.

A **model PASS** requires each R family, and the B08 answer, to meet its
frozen threshold for each provider, model, and mode. No web-only success counts
as a Library success. Report the counts even when the model verdict passes.

Distinguish configuration/prompt defects, provider retrieval failures, model
source-selection failures, evidence failures, and unavailable prerequisites.
This prevents the observed symptom from being waved away as "the store ID was
present" or incorrectly blamed on an upload without evidence.

The final claim is bounded to the recorded build, fixtures, configurations,
models, and trials. It confirms observed behavior across the designed surface;
it does not claim that every possible model response will always choose the
right source.

## 14. Cleanup procedure

Run cleanup in a `finally` path and make it repeatable from `ownership.json`.
Cancel outstanding requests and stop only the test app/processes. Never stop
the user's ordinary application as part of this campaign.

Delete only exact resources recorded as created by this run. Use CHA's
`cloudStore.delete`: for OpenAI it lists and deletes the store's Files before
the store, and for Gemini it deletes the store's documents before the store.
Cancel outstanding upload operations with `notes.storeCancel`. If CHA reports
that the cleanup of a failed or cancelled upload did not complete, retry it
with `notes.storeCleanup`. Then verify
with `cloudStore.get` that each owned store is absent. Retain 404/not-found as
evidence of absence, not as a new failure.

If a create/upload failed after remote allocation but before an ID was
returned, record the uncertainty. Reconcile only with evidence of this run's
ownership; never broadly delete stores selected by a display-name prefix.
Do not erase the ledger until unresolved allocations have been investigated.

Clear borrowed references from the disposable vault without deleting their
remote resources. Confirm the real-book store was never mutated. Remove the
credential-bearing local test vaults, the fixture vault, vault exports, and any
temporary key file. Retain only the redacted evidence package and the explicit
cleanup report. After the final report, delete the campaign files and their
temporary CMake target (section 2).
