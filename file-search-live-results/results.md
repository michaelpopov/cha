# Cross-provider file-search live results

Run on November 1, 2026 (America/Los_Angeles); Observer timestamps range from 2026-11-02T00:29:24.186000+00:00 through 2026-11-02T01:21:26.740000+00:00.

Available core routes delivered real source passages and completed correct answers. The run is **not an all-pass**: two Venice models failed tool usage, Mistral misattributed an omitted source in C15, and the deleted Gemini store did not produce the required C12 store-error report. C03 completion and C16 exhaustion remain unobserved. Missing model/protocol and independent-project prerequisites are listed below.

Build commit: `3d007d2742ff678022034574f3eb6d3d88e3f9dc`, with the user’s pre-existing working-tree changes. [Build-time diff](/Users/mpopov/projects/cha/file-search-live-results/working-tree.patch) and [temporary observer diff](/Users/mpopov/projects/cha/file-search-live-results/observer.patch) record the source state. The observer and disposable CMake target have been removed; both patched sources and CMakeLists.txt match their pre-observer bytes. [Restoration hashes](/Users/mpopov/projects/cha/file-search-live-results/source-restoration.json).

Tests used real `Application` session/settings operations, the production attachment resolver, builders, tool loop, store client, curl transport, and decoders. Disposable blank vaults lived in the existing CHA host directory and reused its OAuth owner. The desktop app remained open on **Michaels**. Only selected provider settings/credentials were copied through the in-process settings path; user chats, notes, and complete vault data were not cloned. Source codes were absent from each scored C01 initial request.

Fixture batches have independent oracles and IDs. Saved key IDs are local to each temporary vault. Within the baseline batch, the same stores and saved-key bindings were reused across four consumers. Later batches supplied Astra OAuth and the explicit Google O-A/G-B variant, scope/failure fixtures, and boundaries. The evidence folders retain each actual folder name, remote ID, saved ID, model, and oracle; logical fixture labels are not global resource IDs.

**Consumer matrix**

| Required role / additional configured consumer | Actual wire model | Protocol / authentication | OpenAI-only | Gemini-only | Mixed | Evidence |
|---|---|---|---|---|---|---|
| Astra through Codex | `gpt-6-astra` | Responses / existing host OAuth | Pass | Pass | Pass | [Astra traces](/Users/mpopov/projects/cha/file-search-live-results/expanded/C01-extra-astra.json) |
| OpenAI API | `gpt-4o` | Direct Responses / saved API key | Pass | Pass | Pass | [Baseline](/Users/mpopov/projects/cha/file-search-live-results/baseline/verification.json) |
| Kimi 3.1 | Not configured | Configured Chat Completions role unavailable | Blocked | Blocked | Blocked | No new provider/model installed |
| Native Anthropic | Not configured | Native Anthropic unavailable | Blocked | Blocked | Blocked | No Anthropic protocol acceptance claimed |
| Google API | `gemini-3.8-flash` | Direct Google / saved API key | Pass: O-A | Pass: G-B | Pass: O-A + G-B | [Variant matrix](/Users/mpopov/projects/cha/file-search-live-results/google-variant/C01-Google-variant-2.json) |
| Available Kimi consumer | `moonshotai/kimi-k2.5` | OpenRouter Chat Completions / saved API key | Pass | Pass | Pass | [Baseline](/Users/mpopov/projects/cha/file-search-live-results/baseline/verification.json) |
| Additional Codex consumer | `gpt-6-luna` | Responses / existing host OAuth | Pass | Pass | Pass | [Baseline](/Users/mpopov/projects/cha/file-search-live-results/baseline/verification.json) |

The required Astra OAuth test profile explicitly selected the existing subscription owner while retaining the configured Astra wire model. Its original saved API-key-to-Codex-URL route was retained as a separate HTTP 403 result in [attempt-1](/Users/mpopov/projects/cha/file-search-live-results/attempt-1/C01-astra-0.json). Original provider definitions were not edited.

Same-vendor tool routes were observed for OpenAI API → OpenAI stores under a different saved ID, Google API → Gemini stores under a different saved ID, and Codex OAuth → OpenAI stores. They advertised `search_attached_files`, contained the expected local target set, and did not also send those stores as native hosted search. Same-value credentials under different IDs still selected the CHA tool. Mixed answers used both services’ source codes, filenames, and independently checked arithmetic.

Across C01 and supported buffered variants, automated evidence checks recorded **34 passes, 6 blocked attempts, and 2 model-usage failures**. These counts exclude missing-role matrix cells and the separate edge cases. [Per-case checks](/Users/mpopov/projects/cha/file-search-live-results/case-verification.json) include complete target sets, saved-ID labels, real outgoing credential comparisons, raw source passages, matching tool outputs, final codes/filenames, arithmetic, and initial-request code absence.

Buffered Responses (`gpt-4o`), Google (`gemini-3.8-flash`), and Chat Completions (`mistral-large-latest`) each completed a real source-based continuation. Buffered Codex OAuth was not forced. The first buffered Kimi attempt was cancelled at the runner’s six-minute bound and remains incomplete; it was not rerun until success. [First buffered outcomes](/Users/mpopov/projects/cha/file-search-live-results/buffered-first/consumer-mapping.json); [Mistral buffered evidence](/Users/mpopov/projects/cha/file-search-live-results/edges/C01-buffered-mistral.json).

**Additional configured connections**

| Configured connection | Requested wire model | Result | Detail |
|---|---|---|---|
| DeepSeek | `deepseek/deepseek-v4-pro-0813` | Pass | Real mixed-store source codes, filenames, and arithmetic verified. |
| GLM | `z-ai/glm-5.1` | Blocked | Inference server returned HTTP 404: No endpoints found for z-ai/glm-5.1. Every candidate endpoint was removed during routing: Filter by Tool Compatibility removed gmicloud/fp8; Filter by Fallback removed chutes/fp8, streamlake/fp8, siliconflow/fp8, phala, atlas-cloud/fp8, alibaba/fp8, novita/fp8, nebius/fp8, baidu/fp8, friendli, z-ai/fp8, venice/fp8. |
| GeminiPro | `gemini-pro-latest` | Pass | Real mixed-store source codes, filenames, and arithmetic verified. |
| QwenLarge | `qwen/qwen3.8-2.4t-a95b` | Pass | Real mixed-store source codes, filenames, and arithmetic verified. |
| Astra | `gpt-6-astra` | Pass | Real mixed-store source codes, filenames, and arithmetic verified. |
| Mistral | `mistral-large-latest` | Pass | Real mixed-store source codes, filenames, and arithmetic verified. |
| Argon | `gemini-4-argon` | Pass | Real mixed-store source codes, filenames, and arithmetic verified. |
| V Abliteration Flash | `abliteration-abliterated-model-large-flash` | Pass | Real mixed-store source codes, filenames, and arithmetic verified. |
| V Aion | `aion-labs-aion-3-0` | Pass | Real mixed-store source codes, filenames, and arithmetic verified. |
| V Venice | `venice-uncensored-1-2` | Fail | Finished after promising to search; zero search calls or dispatches. |
| V RolePlay | `venice-uncensored-role-play` | Fail | Claimed both stores had zero matches; zero search calls or dispatches. |
| V Aion Mini | `aion-labs-aion-3-5-mini` | Pass | Real mixed-store source codes, filenames, and arithmetic verified. |
| FW DeepSeek | `accounts/fireworks/models/deepseek-v4p1-flash` | Pass | Real mixed-store source codes, filenames, and arithmetic verified. |
| FW Qwen | `accounts/fireworks/models/qwen3p8-max` | Pass | Real mixed-store source codes, filenames, and arithmetic verified. |
| FW Kimi | `accounts/fireworks/models/kimi-k2p6` | Blocked | Inference server returned HTTP 404: Model not found, inaccessible, and/or not deployed |
| FW Minimax | `accounts/fireworks/models/minimax-m3` | Pass | Real mixed-store source codes, filenames, and arithmetic verified. |
| FW GLM | `accounts/fireworks/models/glm-5p2` | Blocked | Inference server returned HTTP 404: Model not found, inaccessible, and/or not deployed |
| Cerebras | `qwen-3.8-27b` | Blocked | Inference server returned HTTP 400: {"message":"Invalid fields for schema with types ['string']: {'maxLength'}","type":"invalid_request_error","param":"response_format","code":"wrong_api_format","id":""} |
| V Abliteration | `abliteration-abliterated-model-large-v2` | Pass | Real mixed-store source codes, filenames, and arithmetic verified. |
| Qwen | `qwen/qwen3.8-max` | Pass | Real mixed-store source codes, filenames, and arithmetic verified. |
| Sol | `gpt-5.6-sol` | Blocked | Provider authentication or permission was rejected. |

All first outcomes are retained. HTTP 404 unavailable-model/endpoint responses, the Cerebras unsupported `maxLength` schema keyword, and the API-key Codex-URL 403 are provider/configuration prerequisites or compatibility limitations; no data-delivery pass is inferred from an advertised tool alone. The two Venice prose-only outcomes are **failed usage/reporting cases**, not evidence of retrieval.

**Case and subcase verdicts**

| Case | Verdict | Observed result / remaining boundary |
|---|---|---|
| C01 | Pass on available core routes; additional outcomes above | Real raw-source → normalized result → matching consumer tool result → final answer chain. Missing native Anthropic and exact Kimi 3.1 roles blocked. |
| C02 | Observation only | Paraphrases and a Cyrillic query were accepted; UTF-8 folder/file/source text survived. No ranking benchmark, dated-conflict corpus, or model-quality gate added. |
| C03 | Not observed: complete final answer | Three real dependent searches used opaque aliases from earlier results; the terminal code was delivered in the third tool result. The Kimi continuation generated extensive text and was cancelled at the ten-minute runner bound before finishing. Four receiver calls, 27,242 reported output tokens; first partial result retained. |
| C04 | Pass: four-store routing/data use; blocked: remote project isolation | Two services, four distinct saved key IDs, four source codes/facts. Available selected OpenAI/Gemini keys equal their environment counterparts. Distinct IDs with equal values cannot prove separate-project authorization. |
| C05 | Pass | Character, Forum, and chat sources resolved to the expected union. The shared folder appeared through all three references; each unique store was dispatched once per actual search invocation. Small results were not truncated. |
| C06 | Pass | Multicast Guide received private OpenAI + shared Forum store; Peer received private Gemini + shared Forum store. Their requests/tool results stayed within those target sets; both private codes and the shared code were answered. |
| C07 | Pass | Hidden was never targeted even when its remote ID was in the user query. A new no-attachment chat had an empty target set and no file-search declaration. Query-only schema exposed no arbitrary store/key/URL parameter. |
| C08 | Pass | A successful chat was closed and reopened with retained attachments. A newly uploaded reserved verification code, absent from its prior history/results, was retrieved and answered. |
| C09 | Merged into C01 | Baseline consumer switching reused the same store/key bindings across OpenAI, Google, Kimi, and Codex OAuth receivers. |
| C10 | Pass, both services | Dedicated local key deletion produced explicit omission notices, no dispatch for missing-key stores, healthy-peer answers, and no file-search tool when only missing-key attachments remained. Supplementary context controls directly recorded failure context containing only the healthy folder. |
| C11 | Pass: invalid value / partial failure / same-chat restoration; blocked: valid wrong-project key | OpenAI rejected the invalid saved value with 401; Google with 400. Results and safe warnings identified failures, healthy peers remained usable, and both stores recovered under the same saved ID on the next turn in the same chat. Consumer credentials were unchanged. Separate unauthorized-project credentials unavailable. |
| C12 OpenAI | Pass | The externally deleted store returned 404; CHA emitted a store error and warning and retained healthy results. |
| C12 Gemini | **Fail: stale-store reporting expectation** | After deletion, retrieval returned 200 / STOP without grounding metadata. CHA emitted empty matches without a store error/warning, and the consumer described no matches/relevant files. The missing store was subsequently confirmed 404. This response shape also occurs for a healthy store with no passages: simply changing every empty STOP reply into an error would conflict with C13. No product fix was applied. |
| C13 | Pass: honest missing fact and observed empty handling | CEO birthday was not established by the returned passages and was not invented. Empty STOP/no-metadata shape occurred on a healthy Gemini peer in C12 and produced empty matches without error. That shape does not prove File Search was used or that a fact is exhaustively absent. OpenAI empty `data` was not observed. |
| C14 | Pass: extraction and production setting; diagnostic observed | Only `retrievedContext.text` supplied Gemini matches; source names/passages matched consumer delivery. Retrieval wire model was `gemini-3.8-flash`, `maxOutputTokens: 2048`. The one-token diagnostic returned MAX_TOKENS with no grounding: incomplete retrieval, not successful empty search; it was never fed to a consumer. MAX_TOKENS with useful grounding was not observed. |
| C15 limits | Pass: overall budget, UTF-8/JSON and actual OpenAI/Gemini byte caps | Five targets caused five real dispatches; the first non-fitting entry was retrieved but omitted. Four entries / 53,877 bytes were delivered with `truncated: true`. Larger owned-fixture chunks produced OpenAI assembled texts 8,841–8,891 bytes → 4,095–4,096 bytes and a Gemini chunk 5,159 → 4,096 bytes. Exactly eight C15 stores were created across preparations, including a Google setup rejected for exceeding its 512-token chunk maximum. Production retrieval budgets were unchanged. |
| C15 answer use | **Fail: model source attribution** | Mistral used a retained code but labelled source 0/document 4 as the requested omitted source 4, rather than clearly qualifying the missing Longcase4 evidence. First answer retained. Separate byte-cap subcases correctly reported markers and truncation. |
| C15 unobserved branches | Not observed | No excess-match trimming proof: raw OpenAI `has_more` was false, and a fifth match alone is not proof. OpenAI matches contained one text part, so multi-part joining was not exercised. The non-fitting overall-budget entry was last; no later-target skip beyond it was observed. |
| C16 | Not observed: exhaustion | Eighteen dependent lookup documents plus entry were prepared. Direct calibration returned the terminal code at step 3 (fourth calibration query), before the required budget boundary. No exhaustion pass is claimed. Ordinary Forum traces show the shared allowance of 16; the four-call context was not substituted. |
| C17 | Pass: in-flight cancel and safe continuation | Real bytes-sent event → normal Stop → cancelled transfer/status 0. The old invocation did not dispatch its later Gemini target or continue the model. A new prompt in the same chat retrieved fresh source data; no late completed old answer was appended. Optional pre-dispatch gate not run. |
| C18 | Pass: app guard, notice, context exclusion, reversal | Gemini 2.5 native companion activated omission of OpenAI; explicit notice retained, no OpenAI target/dispatch, no custom search declaration, failure context contained native Gemini only. Removing the companion in a fresh chat restored the real OpenAI tool route and correct answer. Companion native answer content was not scored as retrieval evidence for OpenAI. |
| C19 | Optional; not run | No instruction-bearing document or prompt-injection benchmark added. |
| C20 | Pass: request acceptance and overflow execution | One/two same-key OpenAI stores and same-key Gemini stores used native declarations with empty CHA target sets and no custom file-search declaration/dispatch. Three same-key OpenAI stores yielded two native IDs + third local target; provider accepted the mix and the actual custom call queried the third store with its matching key. Native answer quality was not scored. |

The Gemini 2.5 companion response discussed its Gemini demand source when asked about omitted OpenAI packaging and did not itself identify the application restriction. The explicit application notice and enforced request scope are the C18 evidence; that native response is not counted as an OpenAI source-based answer.

**Evidence, counts, and cleanup**

147 measured receiver-model calls, 85 executed folder-search invocations, and 154 measured store dispatches are recorded. All outgoing store credentials matched the active expected saved values, including deliberately invalid values during C11; this boolean proves selection, not validity or separate-project isolation. Five unrelated summary calls are separately recorded in request statistics, including one in the native Google control. They are not CHA file-search calls.

[Request statistics](/Users/mpopov/projects/cha/file-search-live-results/request-statistics.json) retain per-request counts, usage, elapsed time, outcome and session/request IDs. Reported receiver usage totals 258293 input and 48273 output tokens; cancelled/rejected requests can lack usage, and internal Gemini retrieval usage remains in its raw response bodies. [Run summary](/Users/mpopov/projects/cha/file-search-live-results/run-summary.json).

[Tool-delivery checks](/Users/mpopov/projects/cha/file-search-live-results/tool-delivery-verification.json) matched 84 normalized tool outputs to actual consumer tool-result fields in the corresponding CHA requests. The 85th was intentionally cancelled before continuation in C17. Protocol-specific continuation files in each batch retain real call/result IDs or Google native name/order, and observed replayed thought-signature values match provider-supplied values. Encrypted Responses reasoning was not supplied in these traces. Streaming and buffered acceptance is based on completed source-based conversations.

An independent extraction check matched all 173 returned source matches to raw provider passages (including the specified UTF-8 prefixes), with no generated Gemini text substituted. [Extraction checks](/Users/mpopov/projects/cha/file-search-live-results/extraction-verification.json).

Each batch folder retains `events.jsonl`, `model.log` (and rotated `model.1.log` where present), actual session JSON/Markdown, `consumer-mapping.json`, `ownership.json`, oracle files, and cleanup responses. The saved runner-source text explains fixture/reference setup without credential values. [Byte-boundary measurements](/Users/mpopov/projects/cha/file-search-live-results/result-boundary-verification.json); [empty-result shapes](/Users/mpopov/projects/cha/file-search-live-results/empty-result-observation.json); [curl bytes-sent timing basis](/Users/mpopov/projects/cha/file-search-live-results/curl-bytes-sent-observer-evidence.txt).

All **40 owned stores and 59 owned OpenAI Files (99 resources)** returned **404** on final independent absence checks. The production Google cleanup path removed child documents before deleting each owned store. [Remote absence checks](/Users/mpopov/projects/cha/file-search-live-results/remote-absence.json). All nine temporary vault prefixes and their local keys/references/session sidecars were removed, as were the two directories left by the interrupted preparation. **Michaels remains active**; original provider/key entries and the host OAuth connection were retained. [Local cleanup](/Users/mpopov/projects/cha/file-search-live-results/local-cleanup.json).

Observer source and the disposable CMake target were removed and the library rebuilt. [No observer symbols in rebuilt library](/Users/mpopov/projects/cha/file-search-live-results/build-cleanup-verification.json). The 52 existing folder-store/search unit checks passed after cleanup; live verdicts above derive from live traces. [Unit-check log](/Users/mpopov/projects/cha/file-search-live-results/post-cleanup-unit-tests.log). The retained evidence passed an exact credential-value scan; no Authorization headers or OAuth cache were exported. [Credential audit](/Users/mpopov/projects/cha/file-search-live-results/credential-audit.json).

Execution notes: the initial preparation omitted `open_session` and sent no model request; it was corrected and its owned resources deleted. A later multicast setup lacked the required Character description; the completed results were retained and only unrun cases continued after correction. Bounded execution left the first buffered Kimi attempt and C03 incomplete; no retry-until-pass loop was used. Larger provider chunk settings applied only to owned fixture uploads. No permanent product changes, documentation edits under `docs/`, account creation, or permission changes were made.
