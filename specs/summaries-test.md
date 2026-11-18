# Desktop note summarization acceptance test

**Run the plan in 60 minutes with one tester on one Mac.** The run covers the desktop Notes dialog, native planning, provider requests, editor, SQLite vault, D1 sync records, and a real quit and reopen. The provider responses and vaults are local fixtures. The pass decision covers application behavior, including the text sent to a provider and the document saved from its answer. Live model quality, Windows host behavior, and ChaWeb are outside this run.

## What changed

The reviewed revision is `bafeefce`.

| Commit | Behavior this run must distinguish | Evidence |
| --- | --- | --- |
| `42a60bbf` | One background batch per desktop runtime. The editor stays usable during inference. The owner and vault control cancellation. | A2, B2, C |
| `54b35bdd` | The setup dialog chooses provider, effort, outputs, and Overwrite for one run. Compressed uses the same guarded generation and commit path. | A1–A3, B1–B2, C |
| `bafeefce` | L3 is a saved, independently editable note version. A folder overview uses every eligible child's saved L3, including cached ones. | A1, A4, B1–B3, C, D |
| `9d43bc92`, `220da282` (November 17) | Per-line cost and elapsed time go to the application log. Concurrent starts use bridge connections in one runtime. Vault switching and desktop chat remain usable. | A2–A3, B2, C |

Use the stable L3 design for both target kinds. All four outputs start checked. Overwrite starts on for a note and off for a folder. A failed required L3 suppresses the overview while independent child work continues. The controls design governs the setup dialog and the background design governs the admitted batch. Where the designs differ, the newer design applies: Saved L3, then Controls, then Background lifecycle, then Scheduling and prompts. For example, a note offers L3 and Overwrite applies to L3. [Saved L3](L3-design.md) · [Controls](backctrl-design.md) · [Background lifecycle](backsum-design.md) · [Scheduling and prompts](summary-ext.md)

## Run order

| Minutes | Work and stopping point |
| --- | --- |
| 00–08 | Record the revision and tool versions. Refresh the desktop targets and assets. |
| 08–20 | Run A and B. Check discovery, exit codes, and reports. These commands can run concurrently on the same checkout. |
| 20–30 | Run C, the instrumented native host. Reserve the desktop for this test until its final relaunch exits. |
| 30–49 | Run D1–D6 in order. The fixture setup, UI actions, and database comparisons are included in these 19 minutes. |
| 49–55 | Reproduce the first failure once with the same inputs, or finish a delayed check. Capture both results. |
| 55–60 | Stop the fixture processes, record verdicts, and hand off the evidence. End the run at minute 60. |

Start with the normal development dependencies installed, a configured Ninja build, an interactive macOS login with Xcode command-line tools, Python 3, and the Node version in `webapp/.node-version` (22.23.1 at `bafeefce`). Activate that version in both terminals. The timed run includes incremental builds. At a slot deadline, record any unavailable prerequisite, timeout, or unexecuted case and continue with independent checks. Use the six-minute reserve for a delayed case before calling it blocked. Stop an overrun at the deadline and keep its partial log. Use only the disposable databases created below.

From the repository root, in Bash, create an evidence directory. Use its absolute path for `qa` in each terminal. Enable pipefail in each shell that uses `tee` so a failing command retains its exit status.

```bash
set -o pipefail
export qa="$(mktemp -d "${TMPDIR:-/tmp}/cha-summaries.XXXXXX")"
echo "$qa"
{ git rev-parse HEAD; git status --short; sw_vers; node -v; npm -v; } > "$qa/revision.txt"
cmake --build --preset ninja --target cha_tests cha_app_tests cha_bridge_tests cha_macos_runtime cha_prepare_test_vault
npm --prefix webapp run api-types:check
npm --prefix webapp run build
```

## A. Native service, provider, storage, and transfer

List the matching tests first. Confirm that the list includes `NoteSummaryPlansFolderL3ContributorsAndDueWork`, both cost fixtures, `L3SchemaUpgradePreservesRowsAndReferencesAndRollsBackOnFailure`, `L3GenerationAndManualEditsRoundTrip`, and the chat **Save summary** regressions `BridgeRouterTest.Summary…`. A missing test usually means the binary or filter needs attention. Run only the selected desktop dependencies.

```bash
cases='^(NotesServiceTest|NoteSummaryCostTest|GeminiSummaryCostTest|NoteFolderExportTest|SyncNotesTest)\.'
cases+='|^BridgeRouterTest\.(NoteSummary|NotesFlexTest|CompressesOriginal|CompressionRefusals|CompressionUsesSelectedEffort|SessionSummaryCanGenerate|Summary|BusyVaultLeasePreserves|DesktopSubmitCaptures|ReadOnlyNoteWireContract|NotesRejectStaleVaultContext)'
cases+='|^NoteStorageTest\.(NewDatabaseKeepsSchema2|VersionSchemaUpgrade|LegacyCompressedTextMigration|L3SchemaUpgrade|VersionRowsKeep|CommitLevelValidates|FolderSummaryCreates|FirstAnnotatedFolderSummary|IncomingNoteRecords|IncomingLevelAndExclusion|ReadOnlyNotesSave|L1AudioFollows|CompressionCommitAtomically)'
cases+='|^ApplicationMedia\..*CompressedSummary|^ApplicationVault\.SummaryProviderWorkers|^BridgeProtocol\.EveryMethodMatches|^ProviderClient\.FlexRequires'
ctest --test-dir build/ninja -N -R "$cases" | tee "$qa/native-list.txt"
ctest --test-dir build/ninja -R "$cases" -j2 --no-tests=error --timeout 60 \
  --output-on-failure --output-junit "$qa/native.xml" | tee "$qa/native.log"
```

At `bafeefce` the filter discovers 93 CTest cases. The rows below describe what the selected tests assert. Do not repeat these exercises by hand. The JUnit report shows only test names and failure output. Record each failed test under the row whose scenario it covers, and read its assertions in the linked fixtures. The unit fixtures provide the held requests, second connection, retry hook, database faults, and corrupt transfer files.

| ID | Exercise | Expected observations |
| --- | --- | --- |
| A1 · Admission and wire format | Start with 4,095 and 4,096 bytes of saved original Markdown and a source over 256 KiB. Mix direct, nested, short, excluded, read-only, and generated children. Target an excluded read-only note directly, a managed folder, a missing ID, and an ordinary note named “Summary”. Send missing or extra keys, duplicate/empty/unknown levels, an invalid effort, a string for Overwrite, and a stale epoch. | 4,096 bytes qualifies. A large original reaches inference. A folder admits only eligible direct ordinary notes. A read-only note qualifies and a direct start ignores exclusion. A generated Summary is identified by flag. A note with all selected outputs cached reports “All selected outputs already exist.” An empty eligible folder reports “No eligible notes to summarize.” Rejections make zero generation calls and zero lifecycle Events. A cached folder still needs a valid provider for the overview. A start request requires levels. A successful reply may contain a cached contributor with `levels: []`. |
| A2 · One owner and one terminal Event | Hold a request, compete from the same window and a second bridge connection, finish or disconnect the non-owner, then disconnect the owner. Race owner close against the start Event write. Switch to the same vault, then a different vault with overlapping IDs, and attempt a late commit with the old epoch. Inject start and terminal Event write failures. Run the chat **Save summary** regressions, which share the cancellation path. | Busy identifies the active target. No queued second run. Non-owner cleanup and same-vault selection preserve the batch. Each accepted run normally has one start and exactly one finish **or** cancellation in the originating vault. The start contains target and admitted count. A terminal logging failure still frees the slot. A failed start Event prevents admission. A stale reply cannot write into the new vault. A subsequent valid start succeeds. Chat Save summary keeps its own generation, cancellation and single retirement. |
| A3 · Inputs, retries, Flex and cost | Set Notes to A, choose B and a different effort, change Notes during a retry, then delete B during a held request. Inspect mock transport bodies for Compressed, L1, L2, L3 section and overview. Exercise a retryable 503, exhaustion, refusal, incomplete/empty result, context error, and oversized result. Read the OpenAI and Gemini cost fixtures. | Each note step receives the saved original at step start, stable source instructions and its own configurable prompt. The overview gets section text. B and the chosen effort apply across steps and retries. An already prepared request can finish. The next step using deleted B fails. Retryable failures get up to three waits of 10, 20, 40 seconds via the test hook. Refused or incomplete text never replaces a version. Eligible direct requests carry the provider's Flex field on every attempt. Logs contain one estimate per completed eligible note line, elapsed time, served-tier/cache accounting and the incomplete marker when applicable. Overview and reused sections add no line estimate. Settings → Events has zero line-cost entries. |
| A4 · Persistence and interchange | Open a fresh vault, a legacy compressed-text layout, and the old level-0/1/2 CHECK fixture. Interrupt migration, retry, add L3 and reopen twice. Replace an existing version. Inject a sync queue failure. Apply a generated L3, an edited L3 and missing/null snapshot fields. Export/import a folder, then try an invalid owner, level 4, duplicate L3 or invalid Summary link. | Rollback preserves the old rows and index. Successful migration preserves old IDs, blobs, flags, scroll, linked Summary, L1 audio, foreign keys and the next allocated ID. No L3 is synthesized from an old Summary. A saved L3 has one hidden level-3 row, a separate document ID, independent scroll, and a stable ID on overwrite. Parent deletion removes the row. The version, success Event and outgoing `note.level` record commit atomically. An incoming generated L3 preserves original/L1 audio. An imported note's `note.create` carries the base64-encoded L3 blob. A manual save syncs through the owner's `note.update`. Missing/null `l3_content` clears L3 on apply. Folder transfer remaps the owner and preserves body, position and flags. A rejected import leaves the destination empty. |

Check the mixed plan in `NoteSummaryPlansFolderL3ContributorsAndDueWork`. A has L1 and L3. B has L2. Selecting L1, L2 and L3 with Overwrite off makes only A's L2, B's L1 and B's L3 due, followed by one folder overview. Selecting only L2 makes just A's L2 due and leaves the folder Summary alone. Confirm the positive wire paths in the desktop client fixtures. The part `l3_section` writes a note version, `l3_overview` generates folder text, and `l3` commits the folder document. The note list carries the boolean `has_l3`.

Use the native fixtures to diagnose the failing layer. [Service](../tests/notes/unit_notes_service.cpp) · [Bridge, provider and cost](../tests/bridge/unit_bridge_router.cpp) · [Storage and folder transfer](../tests/storage/unit_note_storage.cpp) · [D1 sync records](../tests/storage/unit_sync.cpp)

## B. Desktop renderer and editor

Run these files as one filtered Vitest invocation. They use deferred provider and save promises to reproduce races at a known point.

```bash
npm --prefix webapp test -- \
  src/api/nativeClient.test.ts src/api/wireFixtures.test.ts \
  src/notes/summaries.test.ts src/notes/summaryImport.test.ts src/notes/epubExport.test.ts \
  src/components/App.summary.test.tsx src/components/Notes.test.tsx \
  src/components/NotesScreen.test.tsx src/components/NotesTree.test.tsx \
  --maxWorkers=2 --reporter=default --reporter=junit \
  --outputFile.junit="$qa/desktop.xml" | tee "$qa/desktop.log"
```

| ID | Exercise | Expected observations |
| --- | --- | --- |
| B1 · Setup and scheduling | Open, alter, cancel/Escape and reopen each target. Remove the saved provider, return an empty list or a settings error, clear every output, double-submit during admission and switch vaults during setup. Run each checkbox alone and mixed cached/missing plans. Hold the first three of five note lines and release them out of order. | Defaults reload from Notes each time. Setup and dismissal make zero requests. Start needs a selected provider and output. Admission errors preserve choices. A stale dialog cannot start work. The old Compress and Cancel compression menu actions are absent. Each active line runs its due Compressed → L1 → L2 → L3 steps in order, with at most three active lines. An unchecked level gets zero calls and writes. A note's L3-only run makes one section call and zero overview calls. For a folder run with L3 checked, every admitted child contributes once in tree order after the lines settle, including children with zero due steps. |
| B2 · Save, focus and ownership races | Fail the initial save or lose the start reply. Hold a provider response until a user edits the source or another note, opens Chat/Settings, or attempts a second start. Hold an autosave on the displayed Compressed/L3 row during its replacement. Repeat on the fallback original, on a pending load, and with a lost commit reply. Display Summary or ANNOTATIONS during folder replacement. Attempt to close CHA during a batch and choose to stay. | A failed initial save keeps the draft and makes zero starts. Lost-start cleanup frees a possibly admitted slot. Provider work leaves navigation and typing available. The old save settles before replacement. Success retires the old buffer. Definite failure retains its editable draft. Fallback edits save to the original ID. On save failure the draft and error stay visible. Navigation wins over a late load. An ordering barrier after a lost required L3 commit cannot confirm that write, even when an older L3 exists. The overview is suppressed. Refresh the currently displayed result or aggregate and preserve foreground focus. Choosing to stay keeps the batch running. |
| B3 · Content, failure and readers | Inspect a rich cached L3, then edit it after its read while the overview is held. Fail L1/L2, a required L3 generation/conversion/commit, a cached read or full-text fallback, overview inference, and oversized final assembly in separate fixtures. Inspect L3 picker, version dot, read-only behavior, audio gate and EPUB. | Read each saved L3 once. Use its text, including comments, for the overview. Copy its top-level nodes, marks and order into the folder document. The later edit belongs to the next run. L2 has a literal title, a generated note L3 stores the body, and the folder has literal H1s around the overview and child bodies. L1/L2 failure permits an otherwise complete folder build. A failed required L3 permits the other children to finish, suppresses the overview, and causes one folder abandon naming the child. Failed overview, conversion, 12 MiB UTF-8 input/final-document overflow, or final storage error preserves the prior folder result. L3 is selectable, shows the actual/fallback tooltip and version dot, and uses the owner's edit permissions. A displayed L3 has no audio player or generation action enabled. L3 mode falling back to a read-only original can play that original's audio. The L3 EPUB has the ` (L3)` suffix, includes full-text fallback per missing chapter, and reports an empty-level error if none have L3. |

The rich-document, editor and storage assertions check different parts of the same operation. In particular, the first annotated folder Summary must create ANNOTATIONS with links to the visible copy. An edit to the copy must leave the source L3 intact. Successful L1 or folder Summary replacement retires its old audio. Fault injection checks the previous Summary, aggregate and audio together. [Coordinator](../webapp/src/notes/summaries.test.ts) · [Editor and setup](../webapp/src/components/App.summary.test.tsx) · [Mode and audio](../webapp/src/components/NotesScreen.test.tsx) · [EPUB](../webapp/src/notes/epubExport.test.ts) · [Storage](../tests/storage/unit_note_storage.cpp)

## C. Real foreground use, quit and reopen

```bash
CHA_NATIVE_ASSETS="$PWD/webapp/dist" python3 -u tests/native/macos/test-note-summaries.py \
  2>&1 | tee "$qa/host.log"
```

Require exit 0 and the final `Verified foreground use, saved versions, and batch cancellation through normal quit and reopen.` line. This harness builds an instrumented production host, serves local HTTP replies, and uses a temporary vault. Its assertions cover these checkpoints.

1. After the fake server receives a held request, the foreground editor saves an unrelated edit. Switch mode and layout, open Events and send a chat with the Compressed attachment. The provider receives the saved Compressed text. Completion leaves focus in the chat composer.
2. A single note saves all four versions. The folder includes the two eligible chapters, excludes the short and excluded notes, and displays the literal headings. Edit a saved chapter L3. The second build sends the edit in the overview input and copies it into the same folder Summary ID.
3. Quit with a child request in flight after earlier commits. Reopen. Check exact HTTP counters, earlier stored versions, prior Summary bytes, and the cancellation Event. The harness also quits during retry backoff, between generation and commit, and after the final folder write while its acknowledgement is held. In the last case, the **committed** replacement survives. Relaunch makes zero spontaneous requests. B2 separately exercises choosing to stay in the app during close negotiation.

[Native host and database assertions](../tests/native/macos/test-note-summaries.py) · [Desktop actions and timing barriers](../tests/native/note-summaries.js)

## D. Follow one folder through reuse, failure and recovery

This is an additional end-to-end check. It correlates the exact HTTP calls, Events and persisted bytes across successive runs on **the same folder**. The fixture below seeds three eligible originals and one 4,095-byte original named X short. A, B and X short already have L3. A contains highlights, a comment, a list and an embedded image. The server gives each answer, including an incomplete one, a unique number, and can return an incomplete answer for B on demand. Setup takes place before opening the disposable profile.

```bash
export db="$qa/profile/test.sqlite3"
./build/ninja/cha_prepare_test_vault "$qa/profile"
python3 -u - "$qa" > "$qa/requests.log" 2>&1 <<'PY' &
from http.server import BaseHTTPRequestHandler, HTTPServer
from itertools import count
import json, pathlib, sqlite3, sys, zlib

root = pathlib.Path(sys.argv[1])
sequence = count(1)

class Provider(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        messages = json.loads(self.rfile.read(int(self.headers['Content-Length'])))['messages']
        body = messages[-1]['content']
        overview = messages[0]['content'].strip() == 'QA overview'
        part = 'overview' if overview else ('section' if body.rstrip().endswith('QA section') else 'unexpected')
        owner = '-' if overview else next((key for key in 'ABC' if 'SOURCE' + key in body), '?')
        reason = 'length' if part == 'section' and owner == 'B' and (root / 'fail-b').exists() else 'stop'
        number = next(sequence)
        print(json.dumps([number, part, owner, reason, body if overview else '']), flush=True)
        answer = {'choices': [{'message': {'role': 'assistant', 'content': f'{part} {owner} {number}'},
                               'finish_reason': reason}]}
        data = json.dumps(answer).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

def paragraph(text):
    return {'type': 'doc', 'content': [{'type': 'paragraph', 'content': [{'type': 'text', 'text': text}]}]}

def blob(doc):
    return zlib.compress(json.dumps(doc, ensure_ascii=False).encode())

server = HTTPServer(('127.0.0.1', 0), Provider)
with sqlite3.connect(root / 'profile/test.sqlite3') as conn:
    for path, value in {
        'system/providers/test/config.toml': f'display_name="QA"\nmode="net"\nhost="127.0.0.1"\nport={server.server_port}\nhttps=false\napi="chat_completions"\nmodel="fixture"\nstream=false\n',
        'system/notes/config.toml': 'compression_provider="test"\ncompression_reasoning_effort="none"\n',
        'system/prompts/summary-l3-section.md': 'QA section',
        'system/prompts/summary-l3-overview.md': 'QA overview',
    }.items():
        conn.execute('INSERT OR REPLACE INTO config(name,content) VALUES (?,?)', (path, value))
    book = conn.execute("INSERT INTO note_items(kind,name) VALUES ('folder','Book')").lastrowid
    rich = json.loads(pathlib.Path('tests/fixtures/notes/annotations.json').read_text())['document']
    png = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jC1sAAAAASUVORK5CYII='
    rich['content'].append({'type': 'image', 'attrs': {'src': 'data:image/png;base64,' + png, 'alt': 'QA pixel'}})
    for key, name in [('B', 'B'), ('C', 'C'), ('A', 'A *literal*'), ('X', 'X short')]:
        source = 'x' * 4095 if key == 'X' else 'SOURCE' + key + ' ' + 'x' * 4096
        owner = conn.execute("INSERT INTO note_items(parent_id,kind,name,content_json) VALUES (?,'note',?,?)",
                             (book, name, blob(paragraph(source)))).lastrowid
        if key != 'C':
            conn.execute("INSERT INTO note_items(parent_id,kind,name,content_json,compressed,level) VALUES (?,'note',?,?,1,3)",
                         (owner, name, blob(rich if key == 'A' else paragraph('Cached ' + key))))
conn.close()
print('READY', flush=True)
server.serve_forever()
PY
provider_pid=$!
```

Wait for `READY` in `$qa/requests.log`. Launch `./scripts/run-native-dev.sh "$qa/profile"` from the root in a second terminal. Use the same absolute `qa` path there. In the first shell, these read-only snapshots make repeated writes and identity changes visible. Save one after each row, for example `snapshot > "$qa/D1.txt"`. If a row requires a before/after comparison, take both. Only compare the relevant rows.

```bash
snapshot() {
  sqlite3 -readonly "$db" "SELECT id,parent_id,name,compressed,level,folder_summary,scroll_top,hex(content_json)
    FROM note_items WHERE kind IN ('note','annotation') ORDER BY id;"
}
events() {
  sqlite3 -readonly "$db" 'SELECT id,severity,message FROM event_history ORDER BY id;'
}
snapshot > "$qa/before.txt"
```

In Notes, open the item's **Actions → Summarize** dialog. For each run choose **only L3**, set Overwrite as indicated, and wait for its terminal Event. Count new lines in the HTTP log since the previous action. Any `unexpected` or unidentified request fails the row. Read A/B/C in tree order even if the server received concurrent section requests in a different order.

| ID | Action | New section / overview requests | Expected persisted result |
| --- | --- | --- | --- |
| D1 | Run **Book**, Overwrite off. | 1 (C) / 1 | Start counts three contributors. A and B keep their bytes and IDs. C gains a hidden L3. One flagged Summary and the first ANNOTATIONS appear. The overview request includes A's comment and B's cached text in order. X short contributes zero calls and zero text even though it has a saved L3. The copy retains the embedded image. Check the rich copy below. |
| D2 | Delete **only the generated Summary** in this disposable Book using its menu. Leave the three saved L3s. Run **Book**, Overwrite off. | 0 / 1 | All-cached children can create a Summary from scratch. Its new ID differs from the deleted one. All child L3 rows match D1. The next successful rebuild targets this new ID. |
| D3 | Run **B** directly, Overwrite off. Then cancel setup after the error. | 0 / 0 | The dialog reports “All selected outputs already exist.” No new start or terminal Event. The previous Summary and B's row match D2. |
| D4 | Run `touch "$qa/fail-b"`. Run **Book**, Overwrite on. | 3 (one each, B incomplete) / 0 | A and C get new bodies at their existing IDs. B keeps its previous bytes. The Summary and ANNOTATIONS match D2 byte-for-byte. Expect a note-level L3 failure for B, one folder failure identifying B, and one ordinary batch finish. No success for B. |
| D5 | Run **Book**, Overwrite off, with the B fault still active. | 0 / 1 | The prior B L3 remains usable. The overview uses the newer A and C plus “Cached B”. The same managed Summary ID gains the new overview. No child version changes. |
| D6 | Move the fault marker aside with `mv "$qa/fail-b" "$qa/fault-used"`. Run **Book**, Overwrite on. | 3 (one each) / 1 | All three versions change at their existing IDs. All three new sections appear once in the Summary, in A/B/C order. The six-row sequence totals **7 section attempts and 4 overviews**, **5 starts and 5 finishes**. Originals match the baseline, and no Compressed, L1 or L2 row appears. |

After D1, run this comparison, then repeat after D2, D5 and D6. It checks every child block, including formatting, against the saved JSON. D4 deliberately retains an older folder Summary, so compare its stored bytes with D2 instead. Save the final Event history with `events > "$qa/events.txt"`.

```bash
python3 - "$db" <<'PY'
import json, sqlite3, sys, zlib
from pathlib import Path

db = sqlite3.connect(Path(sys.argv[1]).as_uri() + '?mode=ro', uri=True)
def blocks(raw):
    return json.loads(zlib.decompress(raw))['content']
def heading(name):
    return {'type': 'heading', 'attrs': {'level': 1}, 'content': [{'type': 'text', 'text': name}]}
folder, raw = db.execute('SELECT parent_id,content_json FROM note_items WHERE folder_summary=1').fetchone()
children = db.execute('''SELECT n.name,v.content_json FROM note_items n JOIN note_items v ON v.parent_id=n.id
    WHERE n.parent_id=? AND n.name<>'X short' AND v.compressed=1 AND v.level=3 ORDER BY n.name''', (folder,)).fetchall()
assert [name for name, _ in children] == ['A *literal*', 'B', 'C']
saved = blocks(raw)
assert saved[0] == heading('Book') and saved[1]['content'][0]['text'].startswith('overview ')
expected = []
for name, body in children:
    expected.extend([heading(name), *blocks(body)])
assert saved[2:] == expected, 'Folder copy differs from the three eligible saved L3s'
print('All three saved L3 bodies and literal chapter headings match.')
PY
```

## Verdict

Record one line for each of A1–A4, B1–B3, C and D1–D6 with the revision, status, evidence filename or test name, expected and observed result, and elapsed time. For an HTTP mismatch include the per-child counts. For a persistence failure include the affected IDs and the before/after snapshot. For cancellation include the originating vault and its terminal Event.

**PASS** requires all listed checks to complete with their expected results, zero failed selected tests, the native relaunch assertion, and the D totals and stored documents to agree. **FAIL** records an observed mismatch, including a failure that passes on rerun. Use **BLOCKED** for required checks that could not execute or complete by the deadline. A green unit suite cannot supply a missing host or recovery result. Quit the dev host, stop its Vite process if still running, and stop the fixture with `kill "$provider_pid"`. Keep the run directory for the report.
