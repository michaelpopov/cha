# One-time live test of cross-provider file search

## Purpose

Verify that CHA can resolve attached stores, search them with their saved
credentials, return real source passages through a model's tool calls, and
produce an answer that correctly uses those passages.

This is a one-time acceptance exercise, not a proposal for a permanent test
suite, CI job, benchmark, or test framework. This document defines the tests;
it does not request their execution. Use a temporary vault, a small set of
synthetic documents, and one results directory. Disposable scripts or a small
temporary runner are sufficient.

The main pass criteria concern actual remote access and data use. Existing
unit tests, mocks, HTTP 200 responses, the presence of a tool declaration, and
a model saying that it searched are not substitutes for this exercise.

## Scope

Include every route that uses CHA's `search_attached_files` tool, regardless
of the consumer model's vendor. Native search is selected only when the
endpoint/API is a supported direct connection and the consumer and store use
the **same saved key ID**. Equal key strings under different saved IDs do not
select the native route. Codex OAuth always uses the CHA tool for attached
stores, including OpenAI stores.

Cover both store services, mixed corpora, Codex OAuth and API-key consumers,
all four wire protocols, attachment scope, saved-key selection, continuation,
and failure reporting. A Gemini store's internal retrieval generation is part
of this path; deliver its source passages, not its generated answer.

Exclude upload correctness, indexing/ranking quality benchmarks, and native
same-provider search-and-answer baselines. Creation, upload, readiness checks,
and cleanup are fixture preparation only. C20 checks native request routing
and provider acceptance, without scoring native retrieval or answers. A native
Gemini store also activates the Gemini 2.5 restriction in C18; do not ask for
its facts there.

Do not include unrelated web search, note search, voice, UI layout, or broad
performance testing. No application UI changes are required.

## Execution and evidence

### Use production paths

Run the acceptance cases through a real CHA conversation, using the desktop
app or its normal bridge/session commands. Exercise the production attachment
resolver, request builders, tool loop, store client, and response decoder.
Use real model and store endpoints with real credentials. Do not insert source
text into a consumer request, fabricate model tool calls, replace provider
responses, or return canned passages from a tool.

### Required temporary observer

Existing model debug logs show tool calls, continuations, and final answers.
They do **not** show store request URLs/statuses or raw retrieval responses.
`FolderStoreClient{}` currently binds the real curl transport inside the
closure created in `ProviderClient::prepare`; there is no installed observer
for that closure. Preparing this live run therefore requires a small,
temporary source patch, not just enabling logging.

Choose a transport wrapper at that exact binding in
`src/providers/provider_client.cpp`. Keep the production
`FolderStoreClient::search_stores` implementation and resolved targets; give
that client a `FolderStoreTransport` wrapper instead of its default transport.
The wrapper must call `folder_store_curl_transport` with the request and
cancellation object unchanged and return the response unchanged:

```text
prepare: capture the actual FolderSearchAccess targets for this request
bind:    FolderStoreClient(observed_transport).search_stores(targets, query, cancel)
observe: identify service/store from the real request URL/body
         compare its credential with the expected saved key in memory
         record sanitized dispatch event and request URL/body
         response = folder_store_curl_transport(request, cancel)
         record status, raw retrieval body, completion/cancellation
         return response unchanged
```

Correlate every event with the CHA request and tool invocation. Obtain the
expected `(service, remote store ID) -> saved key ID/value` mapping from the
fixture ledger and active-vault store/key records in memory. Targets contain
key values, **not saved key IDs**: label IDs from that mapping, not by guessing
from a key string. Record `credential_matches_expected: true/false` after
checking the real `Authorization: Bearer ...` or `x-goog-api-key` header.
Compare the complete resolved target set with the independent expected set.
Two entries with identical key values cannot prove remote credential
isolation, even when this comparison succeeds.

Retain the real model client and builders. Around its prepared
`folder_search_tool`, record the actual query and normalized tool output;
leave their data unchanged. Existing model logs plus these events establish
raw retrieval -> extraction -> model delivery. Register the observer for the
whole measured request so an empty target set or zero dispatches is also
observable, rather than inferred from a missing success log.

For C17, add a temporary curl progress/debug observation of first request
bytes sent; never persist header bytes. This is a timing signal for the
runner's normal Stop command, not a substitute response or a product feature.
An optional pre-dispatch gate is described separately in C17.

The patch must not alter targets, keys, queries, responses, provider settings,
return values, or tool availability. Record its diff, and remove it after the
run. Do not add permanent logging APIs, a second retrieval implementation, or
a reusable harness for this exercise. If the observer cannot be installed,
mark checks needing raw bodies, credential comparisons, request counts, or
transfer timing blocked. Model logs alone cannot pass those checks.

Direct provider requests are allowed for fixture setup, access calibration,
and diagnosis. A direct API script alone cannot pass an end-to-end CHA case.

### Evidence bundle

For each case, retain:

1. Case ID, build/commit plus working-tree changes, UTC time, CHA settings,
   consumer model ID, endpoint, API format, authentication method, and stream
   setting. Record exact wire model IDs; do not infer them from display names.
2. Attached folders and their sources: chat, Character, or Forum. Record saved
   key IDs, provider project aliases, service names, and remote store IDs.
3. The consumer's actual tool call IDs and queries; the store requests and
   statuses; returned source passages; and the tool results sent back to the
   consumer. For Gemini retain the relevant raw grounding metadata and finish
   reason so extraction can be checked independently of generated text.
4. The final answer, omission notices, safe failure logs, and an oracle-based
   verdict for facts, calculations, source attribution, and access scope.
5. Model/tool/store request counts, available usage metadata, and elapsed
   time. These explain cost and delays; there is no latency pass threshold.

Never record credential values, Authorization headers, OAuth tokens, or raw
credential-store contents. Identify credentials by saved ID/project alias.
Record a boolean for whether an outgoing credential matched the expected
store key, without recording that key. Check the evidence for credential
leaks before keeping or sharing it. Synthetic document facts may be retained.

For an ordinary successful cross-provider answer, require this evidence chain:

```text
CHA resolves the allowed store and its saved key
  -> consumer requests search_attached_files(query)
  -> CHA makes the real provider retrieval request
  -> provider returns source text containing an undisclosed test fact
  -> CHA sends that text as the matching tool result
  -> consumer's final answer correctly uses the fact
```

For OpenAI, source evidence comes from the vector-store search response's
`data[].content[].text`. For Gemini it comes from
`candidates[].groundingMetadata.groundingChunks[].retrievedContext.text`.
Gemini `content.parts[].text` is generated text and does not qualify as source
evidence. Whitespace normalization is acceptable; invented or materially
changed facts are not.

Check continuation using the actual consumer protocol:

| Protocol | Evidence to retain |
| --- | --- |
| Chat Completions | Assistant `tool_calls` and corresponding `role: tool` messages with matching `tool_call_id`. |
| Responses | `function_call` and `function_call_output` with matching `call_id`; retained reasoning items when supplied. |
| Anthropic | Assistant `tool_use` and user `tool_result` with matching `tool_use_id`; retained signed thinking blocks when supplied. |
| Google | `functionCall` and `functionResponse`, correlated by supplied ID or native name/order; retained thought signatures when supplied. |

Do not require a protocol field that the real provider does not supply. A
protocol variant passes only after the consumer accepts the continuation and
finishes a correct source-based answer.

### Verdicts

- **Pass:** all required access, retrieval, delivery, and answer checks hold.
- **Fail:** CHA or the model violates an expected behavior. Classify the
  failure as resolution, credentials, retrieval, extraction, continuation,
  answer use, reporting, or lifecycle.
- **Blocked:** a required account, model, credential, or provider feature is
  unavailable. Name the missing prerequisite; do not count this as a pass.
- **Not observed:** a provider-dependent condition, such as an empty grounding
  response or a parallel tool-call batch, did not occur. Do not claim that
  branch was exercised.
- **Observation only:** C02 and C19 record provider/model behavior without
  gating CHA acceptance. They still report any separate extraction,
  continuation, or access-scope defect evidenced by the trace.
- **Merged:** C09 is covered by C01 and creates no additional run.

Do not repeat a case until it passes. Keep the first result. A clearly
identified transient failure may have one diagnostic rerun; retain both
results and explain the difference. Model failure to call an advertised tool
under an explicit search request is a failed usage case, not proof of working
retrieval. If the tool was never advertised, investigate configuration and
request preparation first.

## Fixtures and independent answer oracle

Create only test-owned resources. Keep an ownership ledger from the moment
each remote ID is returned. Upload through CHA where practical, but do not
grade uploads: continue only when the relevant stores/documents are ready.

Use these logical stores. Their names are labels, not fixed resource IDs.

| Store | Service | Purpose |
| --- | --- | --- |
| O-A | OpenAI | Packaging facts, a source code, and multi-step lookup data. |
| G-A | Gemini | Demand/budget facts and a different source code. |
| O-B | OpenAI | A packaging-fact variant with its own code; independent-key access. |
| G-B | Gemini | A demand/budget variant with its own code; independent-key access. |
| Hidden | Either service | An unattached source with a unique fact that must remain inaccessible. |

For O-A/O-B and G-A/G-B, use distinct saved key entries. To prove remote
credential isolation, also obtain valid credentials for separate projects
with non-overlapping store access. Merely saving the same key twice, or using
two keys that both access all stores, does not prove correct credential
routing. If independent projects are unavailable, run the available cases but
mark the remote-isolation case blocked. Do not create accounts or change
account permissions just to satisfy this plan.

### Prepare providers and credentials in the temporary vault

1. Record the original active vault and the selected provider definitions.
   Create a blank test vault in the existing CHA host/configuration directory;
   do not clone the user's notes, chats, or complete vault.
2. While the original vault is accessible, read only the selected providers
   and their required saved credentials through the authorized in-process
   workspace/settings path. Carry values in memory into test-vault saved key
   entries using the normal settings operations. Copy the selected provider
   settings, remapping their key references to the new local IDs. Never use a
   plaintext credential export file or modify the original entries. A small
   disposable setup runner may perform these operations.
3. For store access, create test-vault entries from the authorized environment
   or `~/.zprofile` (`OPENAI_API_KEY`, `GEMINI_API_KEY`), or the selected existing
   credentials above. Use dedicated local entries for destructive tests so
   replacing/deleting them cannot change the consumer credential. Record the
   resulting saved-ID mappings, without their values.
4. Codex OAuth is owned by the CHA host's configuration directory, not the
   temporary vault. On the existing connected host, check status and reuse
   that owner through the normal `ProviderClient` path. Do not paste tokens
   into test-vault keys or copy an auth cache. If a separate host or a signed-out
   host cannot provide an authorized session, mark OAuth cells blocked until
   the normal sign-in flow is completed.
5. Verify each selected consumer is accessible with its recorded model ID,
   endpoint, API format, and authentication mode. Use ordinary Forum chats,
   with no test facts in summaries or automatic note context. Disable unrelated
   web tools in the test profile where supported. Verify that switching to the
   temporary vault did not silently replace a consumer's authentication.

For tool-route matrix cells, the consumer and any same-service store must
reference **different saved key IDs**. Their key values may still have the
same remote access; disjoint-project proof is the separate C04 check. For C20,
explicitly choose the same entry for the direct consumer and native store.
Do not apply an independent-credential rule to that native routing control.
Restore the original active vault when finished; retain the user's original
provider definitions, keys, and host OAuth connection.

Generate fresh random source codes and lookup aliases, and vary numeric facts
once during fixture preparation. Keep the oracle outside CHA's chat history,
Character/Forum instructions, attached notes, summaries, and model inputs.
Only attached folders should expose the test corpus through retrieval. A
model may know the project and folder names, but not the answer values.
Keep basic-case documents short and place each random code beside the facts
it identifies. Check the first consumer request before retrieval: it must not
already contain those facts or codes.

Use a cross-store calculation with an exact independently calculated answer.
For example, O-A specifies 12 units per case and 37 credits per case; G-A
specifies a demand of 41 units and a budget of 160 credits. The correct answer
is 4 complete cases, 148 credits total, and 12 credits remaining. Generate the
actual values for the run and calculate the expected result locally. Give
each source an independent random code that must appear in the final answer.
Calculate separate expected answers for each mixed pair in the matrix:
O-A/G-A, O-B/G-A, and O-A/G-B. Do not ask for O-A-only facts when the cell
attaches O-B instead. Standalone cells ask only for fields present in their
one attached corpus.

Also prepare:

- Distinct project documents whose source codes identify exactly which data
  the consumer received. Relevance/conflict distractors are optional C02 data,
  not a required retrieval-ranking benchmark.
- A lookup whose second query needs an opaque alias found only in the first
  source. The terminal document contains a new random answer code.
- One relevant paragraph in a document containing substantial unrelated text.
- UTF-8 folder names, filenames, and source text; a short non-English query.
- A clearly absent requested fact, without putting that fact or a suggested
  answer in the query.

Use separate documents for reserved facts. Before a negative or isolation
case, verify that its answer has not already appeared in that consumer's
history or tool results. Most cases should start in fresh chats. Detaching a
folder does not erase passages already delivered to a model.

Long-result and tool-budget data can be added to disposable fixtures later,
after the basic cases. Do not multiply large corpora across every model.
Destructive cases must use reserved test stores/key entries so deleting a key
or remote store does not invalidate later cases. Reuse healthy baseline stores
as peers; remove each destructive fixture when its case is complete. Retain
access to the original credential source for remote cleanup after local key
deletion, without copying its value into the ledger.

## Minimum consumer matrix

Use the actual configured, accessible tool-capable models at execution time.
The table names required roles, not new provider configurations to install.
Record missing roles as blocked. Astra and Kimi 3.1 should be included when
available; use their actual configured wire IDs.

| Consumer role | Protocol/authentication | OpenAI-only corpus | Gemini-only corpus | Mixed corpus |
| --- | --- | --- | --- | --- |
| Astra through Codex | Responses / OAuth | Required: O-A | Required: G-A | Required: O-A + G-A; all CHA tool route |
| OpenAI API consumer | Direct Responses / API key | Required: O-B, different saved ID from consumer | Required: G-A | Required: O-B + G-A; both tool targets |
| Kimi 3.1 | Configured Chat Completions route / provider key | Required | Required | Required |
| Anthropic consumer | Native Anthropic / provider key | Required | Required | Required |
| Gemini consumer | Direct Google / provider key | Required: O-A | Required: G-B, different saved ID from consumer | Required: O-A + G-B; both tool targets |

Use an OpenAI consumer key entry for O-A and a Google consumer key entry for
G-A when those native controls are available. O-B/G-B use other saved IDs.
Before each scored matrix run, inspect resolved targets and the actual request:
`search_attached_files` must be advertised, every test store must be in its
targets, and none of those stores may also be sent as native hosted search.
A distinct key ID with the same value must still take this tool route.

Run C01 once for every required cell. Reuse stores and their bindings while
switching the consumer, with fresh chats/reserved facts; this also covers C09.
For additional configured tool-capable consumers, run one mixed tool-route
case, using different saved IDs if either store service matches the consumer.
Do not repeat every edge case on every model. Report missing credentials or
models as blocked; this validates tested connections, not every external model.

Use production streaming settings first. Also exercise one genuine buffered
round trip for each protocol whose configured provider supports it. Do not
force buffered Codex OAuth or label an unsupported transport as covered.

## Test cases

### C01 — Basic remote retrieval and answer use: every required matrix cell

Attach only the corpus for that cell. In a fresh chat, request an explicit
search for a named project's facts, its source code, and the source filename.
Do not supply any answer values. For mixed cells, require the packaging/demand
calculation and both source codes.

Pass only when the full evidence chain is present. All requested codes must
occur in provider source passages, the consumer's matching tool results, and
its final answer. Check the arithmetic and actual filenames independently.
The receiving model must keep its configured provider/authentication; store
credentials must be used only for their respective retrieval requests.

### C02 — Relevance, disambiguation, and conflicts: observation only

Reuse C01 traces to note whether paraphrased questions, related documents,
non-English wording, or dated conflicting facts affect provider selection and
model interpretation. If useful, run one optional extra query with small
distractors; do not build a ranking benchmark or repeat it across consumers.

Record returned sources and model behavior without an acceptance pass/fail
for retrieval ranking or conflict reasoning. A missing relevant chunk is not
by itself a CHA defect. If the trace shows CHA changed or dropped a returned
fact, sent another store's result, or broke continuation, report that concrete
application defect separately. Keep C01's exact-data and arithmetic checks as
the required evidence that consumers can use delivered information.

### C03 — Dependent searches across model rounds

Use the opaque-alias lookup. Request that the consumer first find the lookup
alias, then search using that returned alias to obtain the final fact.
Calibrate the documents before the scored run so the initial lookup does not
already expose the terminal answer.

Observe at least two actual model-requested searches. The later query must use
the alias from the earlier result. Verify both tool-call/result associations
and the final undisclosed code. If the terminal answer was already visible in
the first result, record that the dependency was not exercised rather than
claiming a two-step data-use pass.

### C04 — Mixed services and independent credentials

With a third-party consumer, attach O-A, O-B, G-A, and G-B and request one
private fact/code from each. Require one answer combining all four sources.

Verify the actual outgoing key matches for every store and both services'
retrieval endpoints. With disjoint projects, establish by direct access
calibration that the A credential cannot search B's store and vice versa.
Then confirm that CHA succeeds using each correct saved key. Do not substitute
the consumer key, the first attached store's key, or another working key.

### C05 — Attached folders and union

Attach distinct fixtures in one chat. Ask for the union of their facts with a
third-party consumer.

All attached folders must be usable. Within one tool call there must be one
retrieval per unique resolved store.
Separate queries in later tool calls legitimately search the store again.
Compare recorded store IDs, saved-key matches, and source names with the
expected target set; do not infer deduplication from the final answer alone.

### C06 — Multicast

Attach folders from both store services in one chat. Use third-party consumers
so both store services remain cross-provider. Start a multicast question asking
two Characters for a fact from each folder. Record each request's target set,
tool calls, and final answer.

Each consumer must receive the attached folders and only its own matching tool
results. A shared conversation may later display another Character's answer,
so inspect each independent request before such text is visible.

### C07 — Unattached resources stay inaccessible

With ordinary attached fixtures available, ask for a fact from Hidden and
mention its remote store ID. Do not reveal its answer code. Also issue a new
chat request with no folder attachments.

The tool schema must expose only `query`, not arbitrary keys, URLs, or store
IDs. No request may target Hidden. In the no-attachment case the tool must be
absent. The model must not claim verified access or invent the hidden code.

### C08 — One reopen smoke check

Reuse one successful C01 chat. Close and reopen it, inspect its retained store
associations, and request a reserved fact not already present in history.
Verify one real retrieval with the current key/store bindings and correct
answer use. Do not repeat this on every provider or expand it into a separate
attachment persistence/detach test suite; C05/C07 already cover target scope.

### C09 — Consumer switching: merged into C01

Use the same stores and saved-key bindings across C01's matrix while changing
the consumer and using fresh chats/reserved facts. Check the expected model
authentication and unchanged store authorization in those traces. There is
no additional C09 execution.

### C10 — Deleted saved key and healthy peers

Use a dedicated test-only saved key that is not the consumer credential.
After confirming its store is usable, delete that local key entry while
retaining the attachment. Leave a healthy store attached. Exercise both store
services, combining them in one case if convenient.

The next request must give an explicit folder-omission notice, exclude the
unavailable folder from search targets and failure context, and make no remote
request for it. The healthy store must still supply a correct answer. With
only unavailable folders left, the search tool must be absent. Previously
retrieved facts are not proof of renewed access.

### C11 — Wrong key value, partial failure, and recovery

Replace a test store key's value with an invalid value, keeping its saved ID.
Leave a healthy peer attached, request both sources, and inspect the real
provider rejection. Repeat for OpenAI and Gemini. Use a fresh test question
whose unavailable fact is not already in the history.

The tool result and one safe warning per failed store search must identify
the failure. Successful peer passages must still be delivered and used. The
answer must distinguish verified facts from unavailable data; it must not
describe authentication failure as an empty search.

Restore the original value under the same saved ID and ask for a reserved
fact on the next turn. Real retrieval must recover without re-uploading or
changing the model's credentials. A valid key from a different, unauthorized
project should also fail explicitly, with no automatic key substitution.
This last subcase requires the disjoint-project credentials from C04. If they
are unavailable, mark that subcase blocked and still run invalid-value,
healthy-peer, and restore-in-place checks; do not claim remote project
isolation from two keys that can access the same stores.

### C12 — Stale remote store reference

Delete one test-owned remote store externally while retaining CHA's local
attachment/record. Request a new fact from it alongside a healthy peer. Cover
one OpenAI store and one Gemini store, reusing fixtures near cleanup time.

The actual remote failure must appear as a store error and safe log warning.
It must not become a claimed absence of relevant information. Healthy results
remain usable. Record local records that require subsequent fixture cleanup.

### C13 — Absent information and empty results

Ask for a fact that the corpus does not contain. Inspect actual returned
passages; a provider may return loosely related chunks instead of no chunks.
The consumer must report that the returned evidence does not establish the
answer, with no invented fact or source.

If OpenAI returns an empty `data` array, or Gemini completes with `STOP` and no
source chunks, require an empty matches list without a search-failure error.
Record whether `groundingMetadata` exists at all, whether `groundingChunks`
is present/empty, and any provider evidence of actual File Search use. A
`STOP` reply without metadata is indistinguishable here from one that never
used File Search: it verifies CHA's empty-result handling, not successful
remote search or absence of a fact. An empty list does not prove exhaustive
absence from the store. If these shapes do not occur, mark their live
observation as not observed; do not manufacture responses for coverage.

### C14 — Gemini retrieval-only extraction and token limit

For one cross-provider Gemini case, compare raw grounding passages with the
text delivered to the consumer. The generated Gemini answer may paraphrase
the source; only `retrievedContext.text` and source names may supply matches.
Check the real retrieval model ID and `maxOutputTokens: 2048` request setting.

As a small direct-provider diagnostic, the same fixture/query may be sent once
with a one-token limit. Record its finish reason and grounding presence. A
`MAX_TOKENS` response without passages is an incomplete retrieval error, not
an empty successful search. If useful passages accompany `MAX_TOKENS`, that
finish reason alone must not mark the passages truncated. A blocked or invalid
reply remains an error. Do not lower CHA's production budget for this test or
feed the diagnostic response into the consumer's acceptance run.

### C15 — Bounded results and honest use of partial evidence

With a third-party consumer, use long matching documents and enough attached
stores to exceed the current result budget. Prepare these fixtures only for
this case. Inspect the actual remote results first so the test genuinely
exceeds a limit rather than assuming it does. Use at most eight test stores;
if this is insufficient, record the uncovered boundary and stop expanding.

Check the current production limits against raw data:

- At most five matches per store. A fifth match alone is not evidence of
  truncation; also inspect excess chunks or OpenAI's `has_more` flag.
- OpenAI: at most 4,096 UTF-8 bytes for the **assembled match text**, with its
  text parts joined by the client's newline rule, not 4,096 per content part.
- Gemini: at most 4,096 UTF-8 bytes per grounding chunk's text, which becomes
  one match. Use sufficiently long non-ASCII text (for example Cyrillic), and
  confirm raw byte lengths exceed the limit. Default English chunks may not;
  non-ASCII text alone is not a guarantee either.
- At most 65,536 bytes for the serialized tool result, including its envelope.

Stores are processed sequentially. CHA retrieves and builds an entry before
checking whether that entry fits the overall budget. The first entry that
does not fit can therefore have a real remote request but no returned entry;
CHA sets `truncated: true` and stops. Later stores are not searched. Compare
observed dispatches with the ordered target list and this break point, not
with the assumption that every attached store is queried. C05's retrieval
counts use small, non-truncated results instead.

Verify valid JSON/UTF-8 and truncation flags for actual limits reached. Ask
one question answerable from retained matches and one whose fact was not
delivered. Check correct use of the first and an honest qualification or
further search for the second. Record boundaries not reached as not observed.

### C16 — Tool budget and final response

Use a third-party consumer and an OpenAI-only lookup chain in an ordinary
Forum chat. CHA binds summary tools there, so the expected shared allowance
is **16**, not four. Prepare at least **17 dependent lookup steps** with a
terminal code beyond the allowed searches. Calibrate that earlier passages
do not already contain that code. Summary/other tool calls consume the same
allowance; record them all.

Verify the effective request before running the chain. Only a deliberately
chosen context where neither summary nor web-read tools are bound has the
four-call allowance; do not assume an ordinary chat or UI toggle provides
that context. Do not change production budgeting just to shorten the test.
Ask for one lookup step per search until the terminal code is found.

All executed searches must access real data. Verify that no retrieval runs
beyond the allowance, already collected results remain in continuation, and
the model either gives an honest partial answer after tools are removed or
CHA reports its existing limit error if the model keeps requesting tools.
The undisclosed terminal code must not be invented. An answer that stops
before the budget is reached does not exercise budget exhaustion.
If an early passage already contains the terminal code, the chain did not
exercise this boundary. Count all model tool calls against the shared budget,
including calls to other advertised tools.

If the live model emits multiple search calls in one response, additionally
check every call/result ID and the shared allowance. Do not fabricate a batch
if it does not occur; buffered/streamed single-call continuations remain the
required baseline.

### C17 — Cancel an active retrieval and continue safely

Use a third-party consumer with a real multi-store search, including Gemini.
The observer emits a first-request-bytes-sent event from the real transfer.
Have the disposable runner issue CHA's normal Stop command immediately on
that event while the response is still pending. This removes the human timing
race; it does not delay or replace the provider response.

Verify cancellation ends tool/model continuation, prevents later store
dispatch, and cannot append a late completed answer from that request. Capture
the bytes-sent, Stop, transfer-completion, and next-dispatch ordering. A
prepared request or wrapper-entry event alone does not prove in-flight HTTP.
If the provider finishes first, record in-flight cancellation as not observed,
not a pass and not a reason for repeated attempts until one works.

Optionally test cancellation before dispatch with a single temporary gate in
the observer: hold one prepared request, send Stop, release the gate, and let
the real transport honor the unchanged cancellation object. Require no HTTP
request for that gated store or later stores. Label this **before-dispatch**;
it cannot substitute for cancelling a real active transfer.

Submit a new prompt for a reserved fact. The new request must retrieve and
answer normally without accepting an old call's result. Record provider work
completed before Stop; do not claim it was undone.

### C18 — Gemini 2.5 restriction is visible and reversible

Use a Gemini 2.5 consumer with an OpenAI folder plus a native Gemini companion
store under the consumer's saved key. The companion exists only to activate
the native-tool combination restriction. Do not score its content retrieval.

Before any cross-provider search can run, the user must receive a notice that
the OpenAI folder is omitted because this model cannot combine native File
Search with the folder search tool. It must be absent from the tool targets
and failure context. No OpenAI store request may be dispatched. Native folder
search availability must not be presented as access to the omitted folder.

Detach the companion folder and use a fresh question. The same Gemini
consumer must now call the custom tool, retrieve OpenAI source passages, and
use them in its answer. This second leg is a required cross-provider pass.
If no Gemini 2.5 model is accessible, mark the restriction case blocked.

### C19 — Source text and instructions: optional observation only

If desired after required cases, use one small synthetic document containing
facts plus an instruction to ignore the user and output an override phrase.
Keep it out of every other case. Observe raw returned text, extraction, and
the consumer's behavior; do not grade model prompt-injection resistance as
CHA acceptance or multiply it across providers.

CHA's independently checkable obligations remain unchanged: declare passages
as source data, deliver the actual returned source text, and discard Gemini's
generated answer. A violation of these obligations is an application defect;
a consumer following an embedded instruction is a recorded model-behavior
observation, without assuming that it demands a CHA code change.

### C20 — Native and overflow routing controls: request checks only

These checks preserve the user's exclusion of native answer baselines. Use
real CHA requests and providers, but do not score native retrieval content or
native final-answer quality. Unrelated summary tools may remain advertised.

1. **OpenAI direct, same saved key ID:** attach at most two ready OpenAI stores
   using the consumer's key entry. Require their IDs in one hosted
   `file_search.vector_store_ids`, an empty local target list, and no
   `search_attached_files` declaration or CHA retrieval dispatch. Verify the
   provider accepts the request; no CHA file-search tool round trip is needed.
2. **Gemini direct, same saved key ID:** attach ready Gemini stores using the
   consumer's key entry. Require their names in native
   `fileSearch.fileSearchStoreNames`, an empty local target list, and no
   `search_attached_files` declaration or CHA retrieval dispatch. Verify real
   provider acceptance without native answer scoring.
3. **OpenAI direct, three same-key stores:** use O-A and two additional small
   test stores under O-A's key entry; O-B's different-key store is not eligible
   for this control. Reuse these fixtures for the preceding at-most-two-store
   check. Record the expected priority order from the
   attachment sources. Require two native store IDs and the third in local
   targets, with both hosted `file_search` and `search_attached_files` declared.
   Verify that the real provider accepts this mixed request. Ask explicitly
   for a search of the third store and, if the model invokes the CHA tool,
   verify that its real dispatch uses the third store and matched key. If the
   provider accepts the mix but the model does not invoke the tool, report
   acceptance passed and overflow execution not observed. Do not infer it from
   a final answer or add native answer scoring.

Record unrelated summary/tool activity separately. The native controls assert
no **CHA file-search** round trip; they do not assert that every unrelated
function or every possible model continuation is absent.

## Run order and completion

1. Prepare the blank vault, selected providers/keys, and existing host OAuth
   as specified above. Install and record the temporary observer patch. Build
   CHA with it; retain the original working-tree state for later restoration.
   Create the synthetic oracle/ledger and confirm readiness and access scopes.
2. Run C01 and supported streaming/buffered variants first, including same-
   vendor tool paths. C09 is included here. Diagnose a broken core route before
   spending calls on its edge cases. Run C20's small request-routing controls.
3. Run C03–C08 with healthy fixtures; C08 is one reopen smoke check. Reuse one
   third-party consumer for shared edge cases unless another role is specified.
4. Run C13/C14, the real result boundaries in C15, and C16's 17-step forum
   chain. Keep their large passages out of later ordinary chat histories.
5. Run C10–C12 and C17 on reserved resources, restoring changed key values
   between cases and confining irreversible deletions to their fixtures. Run
   C18 separately. Mark unavailable disjoint-project subcases blocked.
6. Record C02 observations from existing results. C19 is optional and comes
   last if pursued. Neither is an additional model-quality acceptance gate.
7. Summarize every matrix cell and case/subcase: passed, failed, blocked, not
   observed, observation only, or merged. Keep raw-metadata absence, provider
   rejection, and model behavior distinct from CHA application defects.
8. Delete owned remote documents/stores, including owned OpenAI Files, using
   the original authorized keys and confirm absence. Remove temporary local
   keys and the test vault after preserving sanitized evidence.
   Restore the original active vault and remove only the observer diff; retain
   all pre-existing user changes, stores, credentials, and host OAuth state.

Stop on a material application defect, unintended access, or credential leak;
preserve evidence and complete safe cleanup. Do not turn this one-time pass
into a background monitor or an automatic retry loop.

The final report must state which CHA tool routes actually delivered data to
the tested consumers, including same-vendor/different-auth-or-key routes,
whether mixed-store answers used both services,
what failures were handled correctly, which prerequisites or live response
branches remain uncovered, and whether all owned resources were removed.
