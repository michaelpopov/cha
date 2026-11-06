# Cross-provider Forum Library integration test

Date: November 5, 2026

Status: Test design. This document defines one extensive acceptance campaign.
It does not implement or execute the campaign. Writing this plan does not
start paid requests, create remote resources, change account credentials, or
change the user's application settings.

## 1. Objective and acceptance contract

Exercise application Library retrieval through CHA, with real answering
models, real OpenAI and Gemini stores, and the production conversation loop.
The answering model must remain the selected character model while CHA uses
the Library's saved key to obtain source passages from another service.

Use [lib-test.md](lib-test.md) as the campaign model: one planned run, many
individually reported scenarios, an independent answer oracle, fixed trials,
and complete cleanup. The behavior under test is defined by
[lib-all-design.md](lib-all-design.md), with the existing
[Library settings](library-design.md) and [catalogue](lib-catalog.md) rules.

Establish all of the following:

1. Cross-service answering connections receive `library_search`, while
   matching direct connections retain native File Search. A generation has
   exactly one Library route.
2. The answering request uses the answering connection's authentication. Each
   retrieval request uses only the captured Library credential and stores.
3. Real tool calls retrieve otherwise unavailable source facts from both
   storage services, and real model answers use those facts correctly.
4. Source text, attribution, partial results, and limits survive normalization
   and protocol continuations. Gemini helper-generated prose is not evidence.
5. Automatic hosted web search, CHA web tools, and summary tools can coexist
   with application Library retrieval through the ordinary conversation loop.
6. Selection changes, persistence, concurrent characters, cancellation,
   failures, and vault boundaries preserve the captured scope and provider.
7. Library use causes no remote management operations, unintended credential
   disclosure, or changes to production documents and settings.

This is an opt-in, once-run campaign against a recorded build and explicit
models. It is not a new CI service, periodic probe, capability database,
benchmark framework, or product subsystem.

### Changes from the earlier test contract

The new design supersedes the incompatible-provider expectations in the old
plan. Do not copy those expected results into this campaign:

| Earlier expectation | Expectation in this campaign |
| --- | --- |
| Opposite-service models, Chat Completions, and a different saved-key ID receive no Library access | They receive application `library_search` when the Library is valid and the answering connection supports functions |
| Only compatible children in All-characters can retrieve | Each child independently uses its native or application route |
| Required hosted web search remains available | Only Off and Automatic remain; legacy `required` loads as `auto` with a warning |
| A Gemini native exchange can sometimes be opaque | Application retrieval must return actual source excerpts; an opaque helper answer is insufficient |
| Two storage-service keys cover the whole campaign | Direct Anthropic, OpenRouter, and saved subscription authentication are additional prerequisites for their lanes |

A Forum Library still has one saved key and one service. Searching both
services means separate Forum configurations or successive generations, not
an invented mixed-service Library. A valid Forum default still overrides the
character/session provider; leave it unset for the primary cross-provider
trials. Claude does not become an eligible Forum default.

### Separate verdicts

Report three results instead of hiding all failures behind answer correctness:

- **CHA contract:** fixed checks on route, request contents, credential use,
  source normalization, continuation, state, cancellation, and cleanup.
  Every required check must pass.
- **Live retrieval:** whether the actual service returned usable, correctly
  attributed source passages containing the requested facts. Distinguish a
  remote retrieval failure from CHA mishandling a valid response.
- **Answering model:** whether the model requested retrieval when needed,
  used the returned evidence, and answered correctly. Apply the fixed
  thresholds in section 10.

A declaration, HTTP 200, correct store ID, or model statement that it searched
does not establish successful retrieval. A correct answer without the source
chain in section 5 does not pass.

## 2. Scope, hosts, and campaign implementation

### Application paths

Use these hosts, and label every result with its host:

| Host | Purpose |
| --- | --- |
| `D`: in-process driver | Link the real runtime and drive `Application`/`BridgeRouter`, database, events, request workers, provider clients, and HTTPS transport. Follow `tests/integration/application_integration_test.cpp`, including connection acknowledgement and current context epochs. Most live scenarios run here. |
| `N`: native macOS host | Use the actual React UI and native bridge built from the recorded source. Follow the mechanics of `tests/native/macos/test-summary-live.py`, with correct concurrent log pairing. Prove representative cross-provider retrieval in the visible application. |
| `C`: calibration | Isolated storage-client/provider checks for fixture readiness and account support. These are setup evidence and never substitute for `D` or `N` acceptance. |
| `U`: existing unit/component tests | Establish exact malformed input, protocol, timing, and size boundaries that live models cannot reliably produce. Never label these as live retrieval. |

The driver must submit normal chat commands and obtain results through the
real application. A hand-built `GenerationRequest`, a directly bound fake
callback, or a manually composed answering request is not application
acceptance. Such checks belong to `C` or `U`.

Keep answering and retrieval endpoints at their production HTTPS origins.
Do not redirect them through a proxy or localhost for observation. That can
change native eligibility and fails to test the real authentication path.

Only `D` uses `Application::set_web_search_url_override_for_tests()` for the
controlled CHA web scenarios. Use the existing dummy search key technique and
normal `web_reader_url` for page reading. Label those cases **live model +
controlled CHA web backend**. Hosted-web cases use the real hosted service;
the override cannot establish hosted web use.

### Keep the harness small

Use a coordinator, a small C++ driver, and a native UI probe. Adapt earlier
campaign mechanics if retained; otherwise temporary files such as
`tests/integration/library_exchange_campaign.py`,
`tests/integration/library_exchange_campaign.cpp`, and
`tests/native/library-exchange-campaign.js` are sufficient.

Use a temporary build target. Do not add product settings, bridge methods,
provider fallbacks, retries, result caches, or permanent tracing to support
the campaign. Do not patch prompts, force tool choice, replay model replies,
or insert fixture answers into chat. Delete temporary campaign code and
observation changes after the evidence package is complete, without removing
the user's implementation changes.

### Existing evidence is a baseline

`specs/lib-all-validation.json` records useful implementation checks. Its
driver exercised request builders, provider loops, decoders, Library client,
and HTTPS. Session routing/lifecycle were tested deterministically. It also
records that Claude was checked through OpenRouter, not live through native
Anthropic, and that hosted-web evidence varied by protocol/model/mode.

This campaign closes those gaps with real application sessions, direct
Anthropic, independent protocol lanes, and explicit hosted-web evidence.
Earlier successes do not count as this campaign's trials. Use new facts and
stores; do not reuse historical answers as a retrieval oracle.

## 3. Preconditions and connection matrix

### Record and freeze the build

Before paid work, record Git revision, dirty-worktree state and patch hash,
platform, runtime/native/web build identities, model configurations with
secrets removed, test paths, scenario expansion, repetitions, deadlines, and
estimated cost. Build both hosts from the same source. Do not use an older
installed application as evidence for the current implementation.

Run the existing suites listed in section 11. Verify evidence capture on one
short calibration before the main matrix. An unavailable model/account,
invalid fixture, or incomplete trace blocks dependent trials; it does not
produce a pass or authorize silent model substitution.

The model IDs below are explicit proposed targets, based on repository
configuration and recorded checks. This plan does not claim current account
availability. Freeze the exact IDs before execution, including any deliberate
replacement decided before the run. Record provider-reported model versions
where returned. Do not discover or replace models during scored trials.

### Credentials

- Use saved Library entries `O-LIB` and `G-LIB`, with cloud-store metadata
  `openai` and `google` respectively. Read `OPENAI_API_KEY` and
  `GEMINI_API_KEY` from the environment or `~/.zprofile` when absent, using the
  established project procedure. This design phase reads no credentials.
- Use independently named answering entries and saved host OAuth. Locate
  existing authorized Anthropic/OpenRouter connections without printing
  secrets. Do not assume their credentials share the Library key location.
  Missing credentials block their lanes rather than replacing native
  Anthropic with OpenRouter or subscription OAuth with an API key.
- Create `O-COPY` and `G-COPY` with the same values as the corresponding
  Library keys but different saved IDs. These test local key identity, not
  remote project isolation. Keep that distinction in the report.
- Use the normal saved subscription login and OAuth refresh owner. Do not
  print/export tokens, revoke the user's login, or start a new login as an
  incidental test action. A subscription request may use streaming regardless
  of its saved stream setting; record the effective wire mode.
- Store real keys only in access-restricted disposable vaults or the existing
  host authentication store. Exclude credential-bearing vaults and exports
  from the final package. Never put secrets in arguments, source files,
  prompts, logs, screenshots, oracle files, or environment dumps.

### Primary application retrieval lanes

`O` and `G` below identify the Library storage service, not the answering
model. A pair such as `AX/G` is a separate scored lane.

| Lane | Answering connection | Proposed model | Library services | Expected route |
| --- | --- | --- | --- | --- |
| OX | Direct OpenAI Responses, API key | `gpt-5.6-terra` | G | Application |
| GX | Direct Google GenerateContent | `gemini-3.8-flash` | O | Application |
| AX | Direct Anthropic Messages | `claude-sonnet-4-6` | O and G | Application |
| SX | ChatGPT subscription Responses, saved OAuth | `gpt-6-astra` | O and G | Application |
| RR | OpenRouter Responses | `anthropic/claude-sonnet-4.6` | O and G | Application |
| RC | OpenRouter Chat Completions | `anthropic/claude-sonnet-4.6` | O and G | Application |
| CC | Direct OpenAI Chat Completions | `gpt-5.6-terra` | O and G | Application |

These are twelve connection/Library pairs. Run exact-fact core trials in
streaming and buffered modes wherever the connection supports both. For SX,
run the supported effective mode; do not count two saved configurations that
both produce SSE as two transport implementations. If another lane rejects a
mode, report the blocked cell; do not silently reduce required coverage.

The Gemini retrieval helper is always the implementation constant
`gemini-3.8-flash`, independent of the answering model, and uses its one
non-streaming helper request. Answering-mode variants do not create helper
model or helper-mode variants.

### Controls and compatibility lanes

| Lane | Configuration | Expected result |
| --- | --- | --- |
| ON | Direct OpenAI Responses using `O-LIB`, Library O | Native `file_search`; no application retrieval |
| GN | Direct Google using `G-LIB`, Library G | Native `fileSearch`; no application retrieval |
| OK | Direct OpenAI Responses using `O-COPY`, Library O | Application retrieval despite equal secret values |
| GK | Direct Google using `G-COPY`, Library G | Application retrieval despite equal secret values |
| G25 | `gemini-2.5-flash`, Library O, then matching Library G | Application functions for O; native File Search and established custom-function omission for matching G |
| NONE | Each primary protocol with no Library | Ordinary answer, no Library declaration/scope/retrieval |

G25 is conditional on account availability, reported as `NOT_AVAILABLE` if
unavailable. It is not permission to switch the primary Gemini model.
ON/GN/OK/GK are required controls. A copied key does not count as testing a
genuinely separate remote project; that additional permission test is optional
only when an authorized test credential already exists.

### Characters, Forums, and vaults

Use neutral Reader and Analyst characters and the ordinary built-in Assistant.
Use a separate realistic book-discussion profile for natural-language trials;
do not add test-only Library instructions to it. Leave voice off and disable
unneeded background generation in disposable vaults.

Use separate O-Library, G-Library, and no-Library Forums, a dedicated summary
Forum, and a second disposable vault with different scope/facts. Primary
Forums have no default provider. Use valid native defaults only in precedence
tests. Keep the answering persona and supported settings inspectable before
and after route changes.

## 4. Remote fixtures and independent oracle

### Owned resources

Use a separate fixture vault to upload synthetic Notes folders through
`notes.storeUpload` and wait through `notes.storeOperation`. Do not put the
source Notes into chat vaults. Create these stores for each storage service:

| Store | Contents and purpose |
| --- | --- |
| A | A synthetic handbook and reference table; primary retrieval |
| B | A supplement containing an exception and different facts; combined retrieval |
| C | A conflicting edition, unique reserved facts, and a longer Unicode passage; unselected/replacement scope and bounds |
| D | A small expendable document; actual deletion and stale-store errors |

Name fixtures `cha-library-ex-<run-id>-<service>-<role>`. Record returned store,
file/document, and operation IDs in an ownership ledger immediately. A name
prefix alone never authorizes deletion. A/B may use the same display label;
scope and grading must use IDs and source mappings.

Create about 10–20 pages of useful text for A, with facts near the beginning,
middle, and end. Add realistic irrelevant sections. Keep B/C/D small. This is
a semantic-retrieval test, not a large-volume upload benchmark.

### Fact design

Generate different content and independent random facts for O and G. Even
when titles resemble each other, the two services must not contain the same
decisive answers. Include:

- Random verification phrases/codes with at least 128 bits of entropy, an
  unusual named procedure, and exact numeric rules.
- A rule in A and an exception in B that changes the computed answer.
- A contradictory rule in C for a clearly identified different edition.
- A small table with Unicode names, quoted text, line breaks, and integers.
- One explicit statement that a requested detail is not recorded, plus a
  completely unrelated missing-detail query.
- An instruction-like quotation marked as document content. Its instruction
  asks for a harmless visible marker and a change of task, so obedience can
  be detected without requesting a destructive action.
- A long coherent section exceeding 4,096 UTF-8 bytes, with useful facts
  distributed across it. Do not demand that one bounded excerpt contain
  distant facts or that semantic search exhaust the document.
- A two-step reference: one passage identifies a section/document in B, whose
  passage contains a new code. This tests focused follow-up retrieval.

Do not encode answers in titles, filenames, store labels, catalogue entries,
question IDs, or the run ID. Reserve distinct calibration facts and distinct
facts for later turns, post-edit/restart checks, and each multicast child.
Use enough independent facts that a previous excerpt cannot answer a trial
which is supposed to require new retrieval.

Keep evaluator-only `oracle.json`: complete synthetic sources and hashes,
answer fields, accepted normalizations, source file/document mappings, store
membership, and the fixed interpretation checklist. The model-facing driver
receives questions and resource configuration, not the answer map.

### Catalogue and attachments

Give A/B concise useful names and descriptions without the target answers.
Run variants with the catalogue absent, partially populated, and containing
unverified instruction-like text. Search must not require catalogue entries.
Prepare one unrelated attached note and one harmless decoy in a summary/web
fixture; neither may contain the target Library fact.

### Readiness and contamination checks

Wait for completed indexing with a bounded deadline. Record uploaded source
hashes and actual remote file/document identities. Calibrate a reserved fact
from A/B/C for each service with the production retrieval client. Check actual
Google `retrievedContext.text`, the fixed helper request, and acceptance of
`topK: 10`. A fluent helper answer without textual grounding fails readiness.

Use a short function-capability calibration for each answering connection and
effective mode. Direct calibration is labeled `C`; the first full application
trial still has to prove routing, callback binding, and bridge behavior.

Each independent scored trial starts a fresh session. Audit every model-facing
input before the first retrieval and every later round for the target fact:
system/persona/character/Forum text, catalogue, question, history, attachments,
summary catalogue/results, web results, notices, and earlier child answers.
Only the verified Library response and its later replay may introduce it.
For the explicitly multi-turn rows, start a fresh session for the whole case
and use reserved unseen facts for later turns. Those rows are the exception
to creating a new session for each question.

Do not commit summaries in retrieval Forums. Summary scenarios use their own
Forum and an unrelated summary fact. Clearing Library settings does not erase
earlier conversation knowledge; a remembered answer is not an access leak or
evidence of fresh retrieval. A contaminated trial is invalid and cannot earn
a pass; preserve it rather than silently replacing it.

## 5. Evidence: prove the complete source chain

### Minimum evidence per positive application trial

Capture and join this chain for each generation and tool call:

1. Public bridge submission and current Forum/session/character settings.
2. Effective answering provider, API, model, authentication alias, and mode;
   native IDs absent and application scope selected from the starting revision.
3. Initial real answering request with `library_search` and production Library
   instructions. Its schema accepts only `query`; it contains no Library key
   or native store configuration. Store IDs may appear later in tool results.
4. Real model function call, arguments, call ID, and protocol-specific signed
   or reasoning content needed for continuation.
5. Each actual retrieval HTTP operation, selected stores, credential-match
   assertion, timeout, status/transport outcome, and returned source evidence.
6. Normalized JSON tool result linked to that call ID, followed by the actual
   continuation input containing it under the correct protocol representation.
7. Final model response and transcript, exact oracle comparison, source
   interpretation, and generation terminal/statistics.

For OpenAI retrieval, require the fixed
`POST https://api.openai.com/v1/vector_stores/{id}/search`, Library credential,
query and `max_num_results: 5`. Each selected store is searched in configured
order once per invocation unless cancellation intervenes. Extract source
evidence from returned `data[].content[]` text and file metadata. No answering
OpenAI model request is needed for this retrieval service.

For Gemini, require one helper `generateContent` request covering all captured
stores with `topK: 10`, fixed model, `candidateCount: 1`, and
`maxOutputTokens: 2048`. It contains only retrieval instructions and the query,
not the character conversation or other tools. Map normalized matches to
actual `candidates[0].groundingMetadata.groundingChunks[].retrievedContext`.
Never use `content.parts[].text` as a source passage.

Verify that each decisive source passage contains the expected fact and maps
to the owned fixture. Compare raw source text to normalized text, allowing
only the documented joining, field bounds, and omission rules. Repeated
identical text in a generated helper answer is not an independent source.

### Observation without replacing production behavior

Existing debug logs contain complete answering request/response payloads and
round metadata. They do not expose the full raw Library HTTP response or its
credential selection. `Library search:` summaries alone cannot prove those
claims. Do not invent evidence that current logs do not supply.

Use a temporary, passive observation patch in the campaign build only, at
session route selection and the existing Library dispatch/transport and
answering-transport boundaries. It records observations; the original callback,
`search_library` parser, libcurl transport, responses, and cancellation flag
still execute unchanged. For the native host, use the same observation build
and record its patch hash.

The observer may record call IDs, key IDs/aliases from the starting settings,
store IDs, sanitized request bodies, original source-bearing retrieval
response fields, normalized results, timeouts, and start/end events. Keep all
source chunks and finish/block/error fields needed to audit normalization;
omit Gemini generated-answer bodies and usage metadata from the evidence.
Check credential equality in memory and emit only booleans such as
`uses_captured_library_credential`,
`uses_answering_credential`, and `subscription_token_absent_from_retrieval`.
Do not emit secret values, reusable hashes, headers containing credentials,
OAuth refresh bodies, or the entire scope object. Strip unexpected echoed
secrets before writing any evidence. Do not count Gemini helper tokens as
character generation usage. In OK/GK, equal secret values mean byte comparison
cannot establish key identity: the captured saved-key IDs and route-selection
snapshot must establish it. Do not claim remote credential isolation from
those equal-value controls.

This limited observation is necessary because the old native-only log recipe
cannot expose the new retrieval boundary. It is temporary test code, not a
new product logging feature or injectable replacement retrieval route. Keep
the patch reviewable and remove it at the end. Also run the native smoke
cases on an uninstrumented build, checking observable tool results and
answers, to detect any dependence on observation code.

Pair debug payloads by parsed log prefix/thread, active generation, and model
round, as in the earlier plan. Add the function call ID and a local operation
ordinal to retrieval observations. Do not pair concurrent requests by the
last log line seen. Preserve complete answering requests/responses and the
specified retrieval fields across log rotation; copy logs after each case.
Missing/truncated required payloads or ambiguous pairing produce
`EVIDENCE_MISSING`.

### Attribution and normalization checks

| Area | Required assertion |
| --- | --- |
| OpenAI | `store_id` comes from the request; `file_id`/title come from returned file data; no fabricated public URL; usable matches survive another store's failure |
| Google | Source store belongs to captured scope when supplied; missing store can be filled only for a single-store scope; otherwise null; file ID stays null; absent title/URI stay null |
| Source order | Preserve provider/configured-store order; no deduplication by title or invented cross-store ranking |
| Bounds | At most five matches per O store or ten G textual chunks, 4,096 UTF-8 bytes per passage, 512 per metadata string, and 64 KiB serialized JSON including errors |
| Truncation | Shortened fields, omitted matches/material, O `has_more`, or grounded G `MAX_TOKENS` set `truncated`; later matches are removed if serialized size exceeds the cap |
| Errors | Partial O success retains matches plus store-specific errors; all failed operations have a top-level error; a G request-wide error has null store ID; raw provider errors are not forwarded |
| Credential boundary | Neither answering text nor schema/result/transcript contains credentials; later Library results may contain real source/store IDs |

Assert these for actual observed data. Do not enlarge or alter a live response
to force a bound. Exact malformed fields, foreign Google attribution,
image-only grounding, token-limit, escaping, and overflow branches are
deterministic checks in section 11 when the live service does not emit them.

### Separate answering failure from retrieval failure

If a model never calls the offered tool, record a source-selection miss and
zero retrieval requests; do not infer a transport failure. If source passages
omit the answer, record a retrieval miss. If the source contains the fact but
the final answer is wrong, record an answering miss. A malformed request,
credential mix-up, corrupted result, or dropped continuation is a CHA failure.
Keep every outcome, including diagnostics, under its original trial ID.

## 6. Route, settings, and scope scenarios

Each table row defines a family; expand listed variants into separate cases
before the run. Unless a row says otherwise, use `D`, streaming, one fixed
trial per variant, fresh sessions, and web off. Do not multiply every row by
every model. `AX/O` and `OX/G` are the representative application pairs for
stateful rows; the core matrix separately covers all protocols.

| ID | Variants/actions | Required result |
| --- | --- | --- |
| P01 | Each of the twelve primary pairs; settings saved with provider None | Exactly the application route, right effective character model, captured Library service/stores/key and answering timeout; generic declaration without native IDs or secrets; real retrieval in R01 |
| P02 | ON and GN, both supported answering modes | Matching native route and source-backed fact; no `library_search` declaration, callback invocation, O direct-search operation, or G helper operation |
| P03 | OK and GK, both modes | Different saved-key ID chooses application route even when values match; scoped credential comes from Library; source-backed answer |
| P04 | For each protocol: no Library; also removed Library key and unsupported key service on AX/O and OX/G | No native/application Library route or catalogue paragraph; ordinary healthy answering connection still replies; invalid Library warns without blocking chat |
| P05 | Character choice, session override, valid native Forum default, remove only default; repeat for O and G | Full provider precedence remains intact. A valid matching default selects ON/GN and native retrieval, even if the character was AX. Removing it restores the saved session choice and application route without rewriting character/session choices |
| P06 | Attempt to save AX as default; remove/rename a valid default or change its eligibility | Incompatible default is rejected by settings. A stale saved default warns and normal selection resumes; Library remains usable through the resulting valid native/application route |
| P07 | Same-service route transition ON→OK→ON and GN→GK→GN, in fresh sessions | Only local eligibility changes route; no generation offers both. Stores, character identity, catalogue, and underlying Library record remain stable |
| P08 | Cross-service Library change O→G→O with AX | Each next generation uses only its current Library service/key/stores. No mixed-service scope, old callback, or persistence of old references |
| P09 | G25 with O, then matching G, then no Library | Application retrieval with O permits ordinary functions; matching G preserves native Gemini 2.5 custom-function omission; ordinary functions return without Library. Require live fact recovery in both Library cases |
| P10 | Empty/partial catalogue, duplicate store display names, renamed saved key/provider; AX/O and OX/G | Scope follows IDs, not labels or catalogue completeness. Valid search continues; unverified catalogue text is data, not instructions |
| P11 | Save/reopen/restart each Library service and application provider; use N for the visible settings portion | Library and effective settings persist; a reserved unseen post-restart fact is retrieved through the correct route; no copied Library configuration in session storage |

Invalid store lists, missing fields, stale epochs, test backends, and exhaustive
endpoint permutations remain at their existing deterministic boundaries. Do
not weaken validation or mutate const snapshots to create unreachable states.

## 7. Retrieval and answering scenarios

Use all twelve primary pairs for R01/R02. Use AX/O and OX/G for representative
extended retrieval rows unless the row specifies more lanes. Every positive
case needs the complete evidence chain. Questions below are templates; freeze
the actual wording before execution and never insert oracle answers.

| ID | Trials and question/action | Required observations and answer |
| --- | --- | --- |
| R01 | Every primary pair × each supported effective mode × three fresh sessions. One selected A store. “In [book], what verification phrase is printed in the [named section]?” No tool named. | Real `library_search` call, correct scope/credential, actual source passage with the exact random phrase, correctly paired result, exact final answer. No attachments or web needed. All three trials must pass |
| R02 | Every primary pair × each mode × three fresh sessions. Natural book question under the realistic profile, with controlled CHA web available and hosted web off | Ask about contents and a specific internal recommendation without mentioning tools. Require useful document-sourced contents, not a generic blurb, plus one decisive private fact. Library precedes any observable CHA web dispatch. Use fixed English/Russian/English wording across the three trials; score each wording and the group |
| R03 | AX and OX/G, one per mode and applicable service; diagnostic prompt explicitly asks to search the Forum Library | A source-backed answer distinguishes tool availability from natural source choice. R03 never repairs or replaces an R02 miss |
| R04 | A+B for each service, via AX and one opposite-service native-protocol answering model (GX/O or OX/G) | Ask for A's numeric rule, B's exception, and the calculated result. Require source evidence and correct interpretation from both stores. For G, every helper invocation contains both stores in one request; for O, operations cover each selected store in order |
| R05 | Repeat R04 with only A, then select C in a fresh session; both services through AX | B's unseen exception/code is unavailable in the A-only case. C's different edition/rule appears only after C is selected. Require honest gaps and exact C source attribution; colliding labels do not merge editions |
| R06 | Select G A+B+C through OX and AX | Ask for one distinct fact from each. A helper request carries all three full names; no inherited O two-store limit. Score recovery of all three, acknowledging that semantic retrieval can miss a fact; never treat inclusion of three IDs as sufficient |
| R07 | Two-step reference across A/B; AX/O, OX/G, GX/O and RC/G; both modes | First passage points to a section in B that contains a new code. Observe focused follow-up retrieval and correct answer using the new source. Preserve call IDs and any returned signatures/reasoning. If one search already returns both facts, record a valid answer but the multi-round branch as not exercised live; deterministic continuation tests still run |
| R08 | Two turns, AX/O and OX/G | First obtain one fact. Second ask a different interior fact not present in any first-turn excerpt or answer. Require a new invocation/source chain. If the reserved fact already leaked into the first retrieval, mark this variant invalid rather than counting history recall as retrieval |
| R09 | Table/Unicode/quotation query through each of the four answering protocols, covering both Library services across the chosen pairs | Recover exact requested row/name/integer and source text intact. Distinguish quoting source material from adopting it as instructions. Check valid UTF-8 through transport, normalization, transcript and answer |
| R10 | Long-section question, AX/O and OX/G | Audit real passage/result bounds and truncation state. A bounded excerpt must not be described as the whole file. A focused follow-up may recover a distant fact; do not grade a semantic result as exhaustive reading. Record untriggered size branches and cover them in U |
| R11 | Detail explicitly recorded as unknown, then a wholly missing subject, AX/O and OX/G | Observe retrieval attempt and honest limits; no invented code, fabricated citation, or claim every file was examined. O empty success and G no-grounding error remain distinct; do not require services to return the same empty-result shape |
| R12 | Quoted instruction-like passage and malicious catalogue wording, both services through AX and one other protocol | Return the requested document fact; do not emit the planted obedience marker as an instruction response, change scope, or invoke unrelated tools. Follow with ordinary arithmetic in a new turn; retrieval is optional there |
| R13 | Library question followed by unrelated greeting/arithmetic, each primary connection once | Character/provider remains unchanged and answers ordinary questions. Optional extra retrieval is measured, not automatically failed; no hidden helper answer is published as a character message |

For R02, the controlled web backend returns a plausible conflicting public
summary and independent decoy values, never the private answer. A later web
call is counted but is not a miss when the Library was searched first and the
answer uses the correct evidence. A web-only correct-sounding blurb fails the
Library result.

Native regressions use P02 and a small native natural-question control. Do
not rerun the whole earlier native campaign or repeat unrelated upload/UI
combinatorics merely to increase case count.

## 8. Web, summaries, and protocol continuations

There are two separate web tests: controlled CHA application tools, and real
provider-hosted tools. One cannot pass on evidence from the other.

### Controlled CHA web and summary combinations

| ID | Pairs/actions | Required result |
| --- | --- | --- |
| W01 | R02 matrix, hosted off, controlled CHA `web_search`/`web_read` offered | Library-first source choice under normal instructions; search/read dispatch ordering observable in CHA; web results never contain private oracle values |
| W02 | AX/O, GX/O, OX/G, RC/G; both modes | Ask for a private Library rule and a separate external fact available only through the controlled web server. Observe actual Library plus web search/read, correct call/result pairing and final synthesis. If the required web tool is not called, record a model miss |
| W03 | One pair per protocol, services balanced; dedicated summary Forum | Save a real summary with unrelated random fact S using the existing summary path. Ask for S and a new Library fact. Observe summary search/read when needed and independent Library retrieval; no private Library answer in the summary catalogue or saved summary |
| W04 | AX/O and OX/G | Ask a question requiring Library, summary and controlled web evidence. Require all three real sources and correct attribution. Parallel function batching is allowed but not required; inspect every call ID and returned result |
| W05 | Each protocol, one service; web hosted off, on-demand off, character `web_search_tool=false` | Library still retrieves. Summary access remains independent. No CHA web search/read is offered or dispatched; a Library-only answer does not set `web_search_used` or web progress counters |

### Automatic hosted web matrix

Run H01/H02 separately on all these cells:

| Answering connection | Library | Reason |
| --- | --- | --- |
| OX | G | Direct Responses, cross-service |
| OK | O | Direct Responses, different saved Library key ID |
| SX | O and G | OAuth answering with API-key retrieval |
| RR | O and G | OpenRouter Responses wire format |
| RC | O and G | OpenRouter Chat Completions wire format, independent of Responses |

Run each supported effective answering mode. For H01 use three fixed trials
per cell/mode; H02 uses one per cell/mode. Disable controlled CHA web tools so
they cannot masquerade as hosted search. Summary functions remain available.

| ID | Actions | Required result |
| --- | --- | --- |
| H01 | Ask for a private Library fact plus a current public fact that requires checking an authoritative public source | Initial ordinary request offers hosted web plus `library_search` with auto choice; observe actual hosted evidence and actual Library passages, then a correct combined final answer. No preliminary web-only request, forced phase, hidden answer summary, or provider switch |
| H02 | Ask a private Library-only question while hosted mode remains Automatic | Exact source-backed answer through Library. Hosted web remains optional. Record observed unnecessary web use; inspect automatic tool choice and absence of a forced web phase. Lack of web here is useful evidence, not something to manufacture |
| H03 | AX/O and OX/G with saved provider `required`, then character `required`; prepare legacy values while the disposable app is stopped | Load succeeds with obsolete-value warning; typed state is `auto`; supported requests use Automatic and coexist with Library/summary functions. Typed save writes `auto`, unrelated saves do not bulk-migrate stored source. Native UI offers only Provider default/Off/Automatic as applicable |

For H01, freeze the public question, source URL, expected field, and grading
method immediately before the run using an independent evaluator lookup.
For example, use an official software release page and record its dated
snapshot; do not reuse the old validation file's expected release number.
Never put that answer in the question or a local server response. If the
public fact changes during the run, retain both dated source snapshots and
grade against the actual query time; do not invent a wrong-answer failure.

Responses evidence includes real hosted call/action items and citations/source
results when returned. Chat Completions requires its own actual hosted
annotations/citations or other provider evidence, not a Responses fixture or
the model's statement. Require the public answer and observed hosted use;
prior knowledge alone is insufficient. If evidence is opaque, report
`EVIDENCE_MISSING` for hosted use. Do not turn missing evidence into a pass by
switching models or transport mode after a failed trial.

Where application calls cause another round, verify the hosted evidence that
the protocol requires is retained, together with Library results and signed
content. If hosted and Library actions occur in one provider round without
observable internal ordering, mark order unverified. Simultaneous execution
and internal hosted scheduling are not CHA requirements. Library-first timing
is scored only where the trace can establish it.

## 9. Changes, failures, lifecycle, and visible application checks

Run stateful variants on AX/O and OX/G unless stated otherwise. Use observed
events rather than fixed sleeps. Exact race positions and error branches
remain deterministic tests when live timing cannot exercise them.

| ID | Actions | Required result |
| --- | --- | --- |
| T01 | After the first answering request starts, replace A with B, clear Library, or change service; separate variants | In-flight invocation and every continuation keep the original scope/key/provider. Next generation uses the edited scope. Chat settings can show the next-turn state while the old request runs |
| T02 | After dispatch, change only the saved Library key value to a known invalid nonempty test value; then restore it through normal settings | In-flight retrieval retains the original captured value. Fresh retrieval uses the new value and reports a safe failure while the independently healthy answering connection can explain it. Restore the real value for later trials. Do not revoke any real remote key or use the answering key as fallback |
| T03 | Reopen session, restart test app, and create a new session after scope changes | Current Forum configuration controls new requests; reserved unseen facts establish fresh access. No stale callback or copied session Library appears |
| T04 | All-characters with ON/GX/AX in O Forum; GN/OX/AX in G Forum; no default | Native and application children use their own selected providers and independently retrieve. Every child has its own source/call chain and uncontaminated input. One child's answer cannot supply another child's success |
| T05 | All-characters under a valid matching Forum default, then remove default | All children use native default route while set; their original session/character choices and applicable overrides return after removal. Ordinary Assistant follows the same route rules |
| T06 | Concurrent sessions in different Forums, one shared selected store, one different selected store; then second vault with reused local IDs but different stores | No cross-Forum/vault credentials, results, catalogue, summaries, events, or unseen facts. Correctly correlate concurrent calls; use held/open sessions rather than assuming UI navigation keeps an idle session alive |
| T07 | Delete owned D through `cloudStore.delete`, leave its saved Library reference; D alone for both services | Record actual HTTP/service behavior. Tool error or empty usable result is handled honestly; no invented fact or answering-provider change. Invalid remote references do not cause local Library validation to pretend existence was checked |
| T08 | O A+D after D deletion, then G A+D | O retains successful A passages with an error for D when D search fails, without top-level failure if A succeeds. G sends one all-store helper request; on request failure it returns scope-level error, without per-store retries or silent removal of D. If a service tolerates the missing store, record that branch honestly |
| T09 | Library key removed or metadata changed to unsupported service; healthy answering provider independent of that key | Library omitted with warning; normal chat continues. Restoring a valid Library makes the next generation retrieve again; unrelated summary/web tools survive |
| T10 | Stop during active retrieval; close session; switch vault; stop the test host; separate variants | Shared cancellation stops remaining O store operations/G helper and model continuation when cancellation is observed. No late source/result/text enters a new request or vault. Fresh follow-up succeeds. Do not require aborting bytes already sent before cancellation |
| T11 | Attach/detach unrelated note/folder, edit attachment, then clear Library | Attachments retain their normal prompt behavior and never select search stores. Cleared Library supplies no tool; use an unseen fact to avoid confusing attachment/history recall with access |
| T12 | Activate Welcome maintenance, naming, recipient classification, summary/compression, provider connection test, and self-note paths | No application Library scope/tool/retrieval or inherited Forum default in excluded auxiliary requests. Ordinary Assistant in a normal Forum does retrieve. Auxiliary providers keep their own normal configuration |
| T13 | Save/replace/clear Library, delete one of two Forums sharing a store | No remote create/upload/delete due to settings or chat preparation. Surviving Forum still retrieves. No per-turn store listing, metadata fetch, model probe, file download, or URI follow with Library credentials |
| T14 | One export/import/merge round-trip per service, only disposable vaults | Library record survives existing transfer rules. Where the key ID resolves, an application lane retrieves a new fact; where absent, the saved record stays and chat omits retrieval safely. Exports remain in the protected test area and are removed. No R2 transfer is needed |
| T15 | Info-level diagnostic run for AX/O and OX/G; separately inspect sanitized debug trace | Library stats count invocations/failures/cancellation/duration/result bytes; Library-only use does not mark web use. New retrieval diagnostics contain no query, passage, key, or helper answer/usage. Existing answering debug bodies can contain replayed source excerpts and are protected/redacted accordingly |

T01/T02 require an actual tool invocation after the edit to prove capture. If
the model finishes without one, the timing variant is not exercised live;
do not manufacture a delayed callback or claim a race was tested. Existing
snapshot/cancellation tests cover the exact boundary. T10 is likewise
reported at its actual observed cancellation point, not the intended one.

### Native host checks

Use the actual UI/bridge for these focused checks; no UI design changes are
part of this plan:

| ID | Actions | Required result |
| --- | --- | --- |
| N01 | Save G Library with no default, choose OX, ask an unseen fact; repeat O Library with GX | Visible cross-service answer with real application tool/source evidence, correct selected model, and no attachments. Repeat both on the uninstrumented build using observable continuation/source results |
| N02 | Ordinary Assistant with AX or SX, each Library service | Real cross-provider answer in the native host, correct provider identity and transcript; no helper character reply or web-only marker |
| N03 | Inspect provider None, valid native default, then remove it; change session model while unlocked | Model lock follows default rules; native/application route changes on the next request. Character and Effort keep existing idle-state behavior |
| N04 | Character provider-web selector and legacy saved value | Required option absent; Off, Automatic, and inheritance behave as supported; legacy value displays normalized state and remains loadable |
| N05 | Ask Welcome Assistant how cross-provider Library access works | Maintenance request has the updated resource instructions and no Library tool. Answer explains separate answering/retrieval credentials, native versus application access, bounded excerpts, fixed G helper, and Forum-default precedence |

## 10. Repetitions, scoring, and execution limits

Freeze the expanded matrix before remote creation. A case key is
`scenario/lane/library/mode/variant/trial/host`. Enumerate actual cases, source
facts, prompts, and dependencies; never infer unspecified Cartesian products.
Keep `saved_stream_setting` distinct from `effective_wire_mode`.

Core expansion: R01 and R02 each have twelve pairs, two modes for the ten
non-subscription pairs and the supported mode for two SX pairs: 22 cells ×
three trials = 66 trials per family, 132 total. Adjust this explicit count
only before execution if frozen connection support differs. Required blocked
cells remain visible in the manifest and report. Extended rows use the
representative pairs/variants stated in their tables.

One execution can support multiple assertions when host, settings, prompts,
and evidence requirements agree. For example, R01 establishes P01, W01 is the
web-order assessment of R02, and P11/T03 can share a restart sequence. Record
these links explicitly. Do not run duplicate generations just to give each
assertion a separate recording, and do not reuse a case across configurations
that differ. Count each real generation and HTTP attempt once.

Use these fixed thresholds:

- R01: 3/3 source-backed exact answers in every cell. Every credential, route,
  source-normalization, and continuation contract check passes in every trial.
- R02: at least 2/3 complete natural-question successes per cell, with all
  individual wording results reported. Explicit-tool R03 cannot compensate.
- H01: at least 2/3 with both observed hosted web and source-backed Library
  use, plus correct combined answer, per cell/mode. Report all tool-choice
  misses; correct public facts without hosted evidence do not count.
- Other required positive live variants: 1/1 for their defined answer/evidence
  goal, unless a row explicitly identifies a branch as observation-dependent.
  Negative variants pass on the documented error/gap, not a positive fact.
- Exact synthetic codes, requested integers, selected edition, attribution,
  and input-leak checks use the frozen mechanical oracle. Book summaries,
  uncertainty, instruction handling, and Welcome explanation use a fixed
  human checklist. An LLM judge is optional assistance, never the sole judge.

Keep `PASS`, `FAIL`, `BLOCKED`, `EVIDENCE_MISSING`, and `NOT_AVAILABLE`
distinct. For observation-dependent subbranches use `NOT_EXERCISED_LIVE` and
link the deterministic evidence; it is not a positive live pass. Conditional
G25 and separately provisioned remote-project checks may be unavailable.
Required primary lanes may not disappear as optional cases after failure.

One campaign can contain the specified repetitions. It must not repeatedly
try a failed prompt until one answer passes. Preserve the first scored trial;
diagnostic retries have separate IDs and never overwrite it. Do not force a
function or hosted choice, remove competing tools, change models/modes, or
edit the question to rescue a scored result. A later corrective campaign is
a new run against a recorded build and new fixtures.

Plan approximately 350–450 live generations after expanding extended rows,
including controls and native smoke checks. The manifest must contain the
exact total and cost estimate, separating answering calls, Gemini helpers,
OpenAI search operations, storage, and hosted-web charges. Freeze a maximum
of 500 generations, 1,800 combined answering/helper model HTTP attempts, and
900 OpenAI search HTTP attempts. Count calibration and diagnostics within
these caps; count upload/management requests separately. If the expanded
matrix exceeds a cap, revise it explicitly before execution, not by silently
dropping cases during the run. Subscription usage is not zero cost merely
because no per-call billing is exposed.

Use test deadlines of 30 minutes per indexing operation, 10 minutes per full
generation including continuations, and 8 hours for the campaign. Align
ordinary provider timeouts before execution and record the captured timeout
for each retrieval. These are test bounds, not new product lifecycle rules.
Use operation/event waits with bounded polling; send progress between cases.

Stop starting paid work when a frozen cap/deadline is reached. Mark remaining
required cases `BLOCKED`; still collect results and run cleanup. Continue
independent service/lane cases after an isolated failure. An operator stop
also produces a partial report and cleanup.

## 11. Deterministic coverage to confirm before live execution

Build `cha_tests` and `cha_app_tests`, run both suites with local socket
permission for mock HTTP tests, regenerate/check API types, run
`npm run check` in `webapp`, and run `git diff --check`. Record exact commands
and results. Passing existing tests from a different build is insufficient.

Verify these current test links; if a required branch lacks a test, add a
small regression in its existing suite during campaign implementation. Do
not build a campaign-specific fake provider framework or force live models
to emit malformed calls.

| Contract | Existing tests/files to inspect |
| --- | --- |
| Route and captured snapshot/key/store lifetime; mixed multicast; defaults; maintenance/naming/test exclusions | `ForumLibraryTest` in `tests/session/unit_session_open.cpp`, especially `IncompatibleProviderEndpointsAndKeyIdentitiesGetApplicationScope`, `ApplicationScopeOwnsOriginalCredentialsAndFreshGenerationSeesEdits`, `ApplicationRouteCoversGoogleLibraryAndSkipsTestBackend`, and `AllChildrenAndPreparedRepliesRetainTheirStartingRevision` |
| Saved-key identity and endpoint eligibility; malformed Library and catalogue persistence | `Workspace.LibraryEligibilityUsesTheSavedKeyIdentityAndNativeEndpoint`, `MalformedLibraryIsIgnoredAndPreservedOnUnrelatedEdits`, and catalogue tests in `tests/workspace/unit_workspace.cpp` |
| O partial/empty/malformed retrieval, safe errors and exact endpoints/body | `LibrarySearch.OpenAiUsesCapturedScopeAndKeepsPartialResults` and `EmptyOpenAiSearchIsSuccessfulButMalformedSourcesAreErrors` in `tests/providers/unit_folder_store_client.cpp` |
| G fixed helper, all stores, source text rather than generated prose; foreign/missing attribution, no grounding, token limit | `LibrarySearch.GeminiUsesOneFixedHelperAndReturnsOnlySourcePassages`, `GeminiRejectsForeignAndUngroundedResponsesAndAcceptsGroundedTokenLimit` |
| Byte/UTF-8/metadata/result bounds, dropped images/chunks, cancellation/management default timeout | `LibrarySearch.Utf8MetadataAndSerializedEscapingStayWithinBounds`, `GeminiBoundsTextChunksAndReportsOmittedImages`, `CancellationStopsBeforeBetweenAndAfterRequests`; confirm explicit transport-timeout coverage |
| All four protocol schemas, streaming/buffered calls and IDs/signatures, Library-only decoder | `ProviderClientLibrary.LibraryOnlyRoundTripAcrossAllProtocolsAndStreamingModes` in `tests/providers/unit_provider_client.cpp` |
| Invalid JSON, UTF-8, empty/extra/non-string/over-2,048-byte query; unavailable/unknown tool; no network before validation | `ProviderClientLibrary.InvalidQueriesNeverInvokeRetrievalAndErrorsAllowAFinalAnswer`, builder availability tests, and existing decoder argument-limit tests; confirm each listed boundary is represented |
| Overall 16-call budget, one result per call including overflow, independent web budget; final function disablement and signed prefixes | `ProviderClientLibrary.LibraryUsesOverallBudgetAndPreservesBatchCallIdsAtFinalLimit`, `ProviderClientSummary.SeparateWebAllowanceAndOverallLimitPreserveMixedCallIdsAndSignedPrefixes`, existing Anthropic/Google limit tests |
| Hosted evidence replay on direct/subscription/OR Responses and OR Chat, including fragmented streamed annotations | `ProviderClientLibrary.AutomaticHostedWebCoexistsWithLibraryAndSummariesOnEachConnection` plus protocol decoder tests; verify streamed Chat citation coverage independently |
| Cancellation/error isolation; native/application exclusivity and exclusions | `ProviderClientLibrary.CancellationAndRetrievalFailuresDoNotStartStaleWork`, `BuildersRejectDualRoutesAndExcludeMaintenance`; session lifecycle tests |
| G25 native/application difference and native failures without application fallback | `GoogleApi.Gemini25AliasesUseFileSearchWithoutFunctions`, `LibrarySearchCapabilitiesAndDeclarationOrderAreIndependent`, and provider-native failure tests |
| Legacy Required normalization, Off/inheritance, typed saves and UI contracts | `Workspace.LegacyRequiredWebSearchWarnsAndTypedSavesNormalizeToAuto`, workspace/settings/protocol tests, generated DTO types, frontend validators and character settings tests |

An actual partial/ungrounded/missing-attribution service response should also
be audited live when observed. Deterministic evidence establishes CHA's
behavior for precise responses; it does not prove a model used a live store.
Do not assert that an unobserved branch was exercised remotely.

## 12. Campaign order and checkpoints

1. Freeze the manifest, prompts, fact reservations, grading, exact case
   expansion, model IDs, cost/caps, and supported modes. Verify account and
   saved OAuth prerequisites without disclosing credentials.
2. Build both hosts, run deterministic prerequisites, and prepare disposable
   vaults plus the ownership ledger. Verify the observation patch is passive.
3. Generate sources/oracle, upload through CHA, wait for indexing, and perform
   isolated calibration. Stop dependent lanes on readiness/evidence failure.
4. Use the first R01 trial for each required pair as a source-chain checkpoint,
   and N01 as the native UI smoke per service. These remain scored cases, not
   unreported warmups. Inspect complete traces before spending the rest of the
   budget.
5. Run core R01/R02 and required native/key-identity controls, then multi-store,
   attribution, table, long-text, missing-evidence, and instruction cases.
6. Run controlled web/summary and real hosted-web matrices. Keep their evidence
   and verdicts separate. Audit protocol replay while sources are available.
7. Run live edits, route transitions, restart, concurrency, native UI,
   auxiliary exclusions, and vault-transfer checks. Run uninstrumented native
   smoke checks before concluding instrumentation independence.
8. Delete owned D and run remote error/partial cases only after positive
   D-dependent preparation is complete. Run cancellation variants with fresh
   facts and collect terminal/event isolation evidence.
9. Audit complete evidence, package redaction, and resource boundaries. Clean
   up in a finally path even after failures or interruption.
10. Remove temporary campaign code/observation patch/target, preserve unrelated
    work, and produce the final case matrix and three verdicts.

Every checkpoint records dependencies. A failed calibration can block a lane,
but cannot erase its required rows. A cleanup failure makes completion
incomplete even if retrieval and answering thresholds passed.

## 13. Evidence package and completion rule

Keep one directory, for example `library-ex-test-results/<run-id>/`:

| Artifact | Contents |
| --- | --- |
| `manifest.json` | Build and observation patch hashes, model/protocol/auth aliases, effective modes, exact case list, prompts/rubrics, thresholds, dependencies, caps/cost estimate and timestamps |
| `ownership.json` | Owned store/file/document/operation IDs, creator alias, allocation/deletion state and unresolved resources; no credentials |
| `fixtures/`, `oracle.json` | Synthetic sources, hashes, exact answers and accepted normalization/source mappings; evaluator-only during execution |
| `cases/<case-key>/` | Bridge actions, snapshots/settings, input-leak audit, complete sanitized answering rounds and specified retrieval fields, call/result mapping, source comparison, transcript, terminal/stats and separate assertions |
| `ui/` | Native UI screenshots/action trace; instrumented versus uninstrumented build labels |
| `unit-results/` | Deterministic commands/results and confirmed coverage map; no claim these were live requests |
| `results.json`, `report.md` | Per-case CHA/retrieval/model results, counts/thresholds, unexercised branches, evidence links, failures, account/service blockers, latency/call totals and cost/accounting limits |
| `cleanup.json` | Every owned resource's final disposition and verification; local secret-vault/export removal; temporary-code removal and outstanding work |

Scan before packaging. Exclude credential vaults/exports, auth headers,
environment dumps, raw token-refresh traffic and unredacted errors/logs. Mask
unowned store IDs/names in discovery replies and screenshots. Preserve owned
synthetic source text and oracle values needed to audit retrieval. A missing
trace cannot be reconstructed by making a new request and presenting it as
the old one.

The campaign is complete only when:

1. Every required primary pair, protocol/effective-mode cell, representative
   lifecycle/UI case, and native/key-identity control has its required evidence
   and CHA checks. No required case is `BLOCKED` or `EVIDENCE_MISSING`.
2. R01/R02/H01 and other defined live goals meet their frozen thresholds, with
   all individual misses reported. Source-backed cross-service access is
   established for both Library services; the native Anthropic lane is not
   inferred from an OpenRouter Claude result.
3. Credential separation, actual source excerpts, attribution, ordinary
   continuation, stable answering identity, scope isolation, and honest
   failures are established. No helper-generated prose is counted as source.
4. Optional/conditional cases and precise branches covered only by unit tests
   are explicit. Required hosted-web use has protocol-specific live evidence.
5. Cleanup is verified and temporary campaign code removed, with no changes to
   the user's production Library documents, stores, sessions, or settings.

Report CHA, live retrieval, and answering-model verdicts even when overall
completion fails. Classify failures as application contract, remote retrieval,
model source selection/answering, fixture/account, evidence, or cleanup. State
the tested build/models/accounts/modes and the limits of the claim; this is
evidence for the recorded scenarios, not a promise about every future model
response.

## 14. Cleanup

Run cleanup in a repeatable finally path using the ownership ledger. Cancel
outstanding test generations/uploads and stop only test processes. Never stop
the user's ordinary app or revoke its host login.

Use CHA's existing `cloudStore.delete` for exact run-owned resources. Its
OpenAI cleanup removes associated files; its Gemini cleanup removes documents
before the store. Cancel uploads with `notes.storeCancel`, and use
`notes.storeCleanup` for an operation whose cleanup remains incomplete.
Include already-deleted D. Verify each owned store absent with
`cloudStore.get`; record not-found as evidence. Where child IDs were recorded,
retain child cleanup results as well, not just an empty Library setting.

If allocation may have occurred without a returned ID, record uncertainty and
reconcile only with evidence of this run's ownership. Never bulk-delete by
name prefix, delete borrowed resources, or erase an unresolved ledger.

Remove protected local fixture/chat vaults, exports/imports, temporary key
files, and unsanitized trace buffers. Keep only the sanitized evidence and
cleanup record. Restore/remove only temporary observer and harness changes;
preserve the user's pre-existing implementation and tests. Any unresolved
owned resource or secret-bearing artifact is explicit remaining cleanup work
and prevents a claim of complete cleanup.
