#!/usr/bin/env python3
"""One-time, paid summary experiment through the production native host.

Run with the user's login environment. Evidence contains no vault or key rows.
Use --lane NAME to run a single lane; model IDs are explicit and never replaced.
Add --natural to ask for the code without naming tools or prescribing retrieval.
"""
import argparse
from contextlib import closing
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import secrets
import signal
import sqlite3
import subprocess
import tempfile
import urllib.error
import urllib.request
import zlib

REPOSITORY = Path(__file__).resolve().parents[3]
LANES = [
    dict(name='openai', key='OPENAI_API_KEY', host='api.openai.com', api='responses',
         model='gpt-5.6-terra', stream=True, base_path='', primary=True),
    dict(name='openai-chat', key='OPENAI_API_KEY', host='api.openai.com', api='chat_completions',
         model='gpt-5.6-terra', stream=False, base_path=''),
    dict(name='gemini', key='GEMINI_API_KEY', host='generativelanguage.googleapis.com', api='google',
         model='gemini-2.5-flash', stream=True, base_path='/v1beta'),
    dict(name='xai', key='XAI_API_KEY', host='api.x.ai', api='chat_completions',
         model='grok-4.20-0309-non-reasoning', stream=True, base_path=''),
    dict(name='openrouter', key='OPENROUTER_API_KEY', host='openrouter.ai', api='chat_completions',
         model='anthropic/claude-sonnet-4.5', stream=False, base_path='/api'),
    dict(name='anthropic', key='ANTHROPIC_API_KEY', host='api.anthropic.com', api='anthropic',
         model='claude-sonnet-4-5', stream=True, base_path=''),
]


def dump(value):
    return json.dumps(value, ensure_ascii=False)


def redact(text, keys):
    for key in keys:
        if key:
            text = text.replace(key, '[REDACTED]').replace(json.dumps(key)[1:-1], '[REDACTED]')
    return text


def verify_model(lane, key):
    """Read account model lists, without retaining credentials or response headers."""
    if lane['api'] == 'google':
        url = 'https://' + lane['host'] + '/v1beta/models?pageSize=1000'
        headers = {'x-goog-api-key': key}
    elif lane['api'] == 'anthropic':
        url = 'https://' + lane['host'] + '/v1/models?limit=1000'
        headers = {'x-api-key': key, 'anthropic-version': '2023-06-01'}
    else:
        url = 'https://' + lane['host'] + lane['base_path'] + '/v1/models'
        headers = {'Authorization': 'Bearer ' + key}
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=45) as response:
        body = json.load(response)
    ids = [item.get('id', item.get('name', '').removeprefix('models/'))
           for item in body.get('data', body.get('models', []))]
    if lane['model'] not in ids:
        raise ValueError('Configured model is not in the account model list: ' + lane['model'])
    return dict(url=url, model=lane['model'], listed=True)


def configure(root, lane, key):
    subprocess.run([str(REPOSITORY / 'build/ninja/cha_prepare_test_vault'), str(root)],
                   check=True, capture_output=True, text=True)
    settings = dict(display_name='Live test', mode='net', host=lane['host'], port=443,
                    https=True, api=lane['api'], model=lane['model'], stream=lane['stream'],
                    base_path=lane['base_path'], api_key='api_key_1', web_search='off',
                    reasoning_effort='none', timeout_s=180, idle_timeout_s=90)
    # Anthropic requires max_tokens. Use its adapter's default 64,000 allowance.
    def toml(values):
        return ''.join(name + ' = ' + dump(value) + '\n'
                       for name, value in values.items())
    with closing(sqlite3.connect(root / 'test.sqlite3')) as database, database:
        rows = {
            'system/keys/api_key_1/config.toml': toml(dict(display_name='Live test', type='models', value=key)),
            'system/keys/config.toml': 'next_id = 2\n',
            'system/providers/live_reader/config.toml': toml(settings),
            'system/providers/live_notes/config.toml': toml({**settings, 'display_name': 'Live Notes'}),
            'system/notes/config.toml': 'compression_provider = "live_notes"\ncompression_reasoning_effort = "none"\n',
            'system/session/config.toml': 'naming_provider = "test"\n',
            'characters/live_reader/character.toml': 'display_name = "Live Reader"\nprovider = "live_reader"\n',
            'characters/live_reader/CHARACTER.md': 'Follow the user request.\n',
            'forums/lobby/members/live_reader/character.toml': '# ordinary forum member\n',
            'forums/lobby/config.toml': 'display_name = "The Lobby"\ndefault_character = "guide"\n',
        }
        database.executemany('INSERT OR REPLACE INTO config(name,content) VALUES (?,?)', rows.items())
    (root / 'app.toml').write_text('vault = "Test"\n[logging]\nfile = "runtime.log"\nlevel = "debug"\n')
    return {name: value for name, value in settings.items() if name != 'api_key'}


def documents(root):
    with closing(sqlite3.connect('file:' + str(root / 'test.sqlite3') + '?mode=ro', uri=True)) as db:
        db.row_factory = sqlite3.Row
        rows = db.execute('''SELECT n.id,n.name,n.read_only,n.compressed_text,n.content_json,
            s.session_key,c.session_id,f.forum_id,p.forum_id AS parent_forum,r.forum_id AS root_forum,
            r.parent_id AS root_parent FROM note_summaries s JOIN note_items n ON n.id=s.note_id
            JOIN sessions c ON c.session_key=s.session_key JOIN forums f ON f.forum_key=c.forum_key
            JOIN note_items p ON p.id=n.parent_id JOIN note_items r ON r.id=p.parent_id''').fetchall()
        result = []
        for row in rows:
            item = dict(row)
            blob = item.pop('content_json')
            if isinstance(blob, bytes):
                try:
                    blob = zlib.decompress(blob)
                except zlib.error:
                    pass
            item['document'] = json.loads(blob)
            result.append(item)
        return result


def read_rounds(logs):
    """Recover complete debug payloads in chronological rotation order."""
    rounds = []
    for filename, content in logs:
        for line in content.splitlines():
            for event, field in [('HTTP model request body', 'request'), ('HTTP model raw response', 'raw')]:
                marker = event + ' data='
                if marker not in line:
                    continue
                payload = json.loads(line.split(marker, 1)[1])
                if field == 'request':
                    rounds.append(dict(request=json.loads(payload), raw='', log=filename, order=len(rounds) + 1))
                elif rounds:
                    rounds[-1]['raw'] = payload
    return rounds


def response_objects(raw):
    if raw.lstrip().startswith('{') or raw.lstrip().startswith('['):
        try:
            value = json.loads(raw)
            return value if isinstance(value, list) else [value]
        except json.JSONDecodeError:
            pass
    result = []
    for line in raw.splitlines():
        if line.startswith('data:') and line[5:].strip() != '[DONE]':
            try:
                result.append(json.loads(line[5:].strip()))
            except json.JSONDecodeError:
                pass
    return result


def generated_markdown(api, raw):
    objects = response_objects(raw)
    if api == 'responses':
        completed = [obj['response'] for obj in objects if obj.get('type') == 'response.completed']
        if completed:
            return ''.join(part.get('text', '') for item in completed[-1].get('output', [])
                           for part in item.get('content', []) if part.get('type') == 'output_text')
        return ''.join(obj.get('delta', '') for obj in objects if obj.get('type') == 'response.output_text.delta')
    if api == 'chat_completions':
        return ''.join(choice.get('message', choice.get('delta', {})).get('content') or ''
                       for obj in objects for choice in obj.get('choices', []))
    if api == 'google':
        return ''.join(part.get('text', '') for obj in objects for candidate in obj.get('candidates', [])
                       for part in candidate.get('content', {}).get('parts', []) if not part.get('thought'))
    return ''.join(obj.get('delta', {}).get('text', '') for obj in objects)


def system_text(api, request):
    if api == 'responses':
        return request.get('instructions', '')
    if api == 'chat_completions':
        return '\n'.join(message.get('content', '') for message in request.get('messages', [])
                         if message.get('role') == 'system')
    if api == 'google':
        return '\n'.join(part.get('text', '') for part in request.get('systemInstruction', {}).get('parts', []))
    value = request.get('system', '')
    return value if isinstance(value, str) else '\n'.join(part.get('text', '') for part in value)


def tool_declarations(api, request):
    if api == 'google':
        return [function for tool in request.get('tools', []) for function in tool.get('functionDeclarations', [])]
    if api == 'chat_completions':
        return [tool.get('function', {}) for tool in request.get('tools', [])]
    return request.get('tools', [])


def verify_continuations(api, rounds, scenario, note_id, tag, code):
    """Inspect the actual adapter-specific call/result pairs, including their IDs."""
    found = []
    for index, round_ in enumerate(rounds):
        body = round_['request']
        calls, results = [], []
        if api == 'responses':
            calls = [(item['name'], json.loads(item['arguments']), item['call_id'])
                     for item in body.get('input', []) if item.get('type') == 'function_call']
            results = [(item['call_id'], item['output']) for item in body.get('input', [])
                       if item.get('type') == 'function_call_output']
        elif api == 'chat_completions':
            calls = [(call['function']['name'], json.loads(call['function']['arguments']), call['id'])
                     for message in body.get('messages', []) for call in message.get('tool_calls', [])]
            results = [(message.get('tool_call_id'), message['content']) for message in body.get('messages', [])
                       if message.get('role') == 'tool']
        elif api == 'google':
            calls = [(part['functionCall']['name'], part['functionCall'].get('args', {}), part['functionCall'].get('id'))
                     for message in body.get('contents', []) for part in message.get('parts', []) if 'functionCall' in part]
            results = [(part['functionResponse'].get('id'), dump(part['functionResponse']['response']),
                        part['functionResponse']['name']) for message in body.get('contents', [])
                       for part in message.get('parts', []) if 'functionResponse' in part]
        else:
            calls = [(part['name'], part['input'], part['id']) for message in body.get('messages', [])
                     for part in message.get('content', []) if isinstance(part, dict) and part.get('type') == 'tool_use']
            results = [(part['tool_use_id'], part['content']) for message in body.get('messages', [])
                       for part in message.get('content', []) if isinstance(part, dict) and part.get('type') == 'tool_result']
        for name, args, call_id in calls:
            if name not in ('summary_search', 'summary_read'):
                continue
            if name == 'summary_search':
                if scenario == 'natural':
                    if not isinstance(args.get('query'), str) or not args['query'].strip():
                        continue
                elif args != dict(query=tag, before_note_id=None):
                    continue
            if name == 'summary_read' and args != dict(note_id=note_id):
                continue
            prior = rounds[:index]
            # A continuation alone is insufficient: the raw provider reply must contain the call.
            observed = any(name in item['raw'] and (call_id is None or call_id in item['raw']) for item in prior)
            matching = [item[1] for item in results if item[0] == call_id
                        and (api != 'google' or item[2] == name)]
            if not observed or not matching:
                continue
            for result in matching:
                parsed = json.loads(result)
                if name == 'summary_search':
                    valid = any(item.get('note_id') == note_id for item in parsed.get('items', []))
                    if scenario == 'natural':
                        valid = valid and code not in result
                else:
                    valid = parsed.get('note_id') == note_id and code in parsed.get('text', '') \
                        and 'User comments added to this note' in parsed.get('text', '') \
                        and 'User comment:' in parsed.get('text', '')
                if valid and name not in found:
                    found.append(name)
    expected = ['summary_read'] if scenario == 'direct' else ['summary_search', 'summary_read']
    return found == expected, found


def inspect(lane, capture, rounds):
    checks = []
    def check(label, condition):
        checks.append(dict(check=label, passed=bool(condition)))
    api = lane['api']
    check('UI workflow completed', capture.get('ok'))
    saved = capture.get('saved', [])
    check('One linked, read-only summary in the managed source forum folder', len(saved) == 1
          and saved[0]['read_only'] == 1 and saved[0]['parent_forum'] == 'lobby'
          and saved[0]['root_forum'] == '' and saved[0]['root_parent'] is None
          and saved[0]['forum_id'] == 'lobby'
          and saved[0]['session_id'] in capture.get('sourceSession', ''))
    comments = []
    def without_comment(node):
        if node.get('type') == 'paragraph' and capture.get('code', '\0') in dump(node):
            comments.append(node)
            return None
        result = dict(node)
        if 'content' in node:
            result['content'] = [child for child in (without_comment(item) for item in node['content']) if child is not None]
        return result
    body = without_comment(capture.get('afterComment', {}))
    check('Exactly one later comment added; generated summary body unchanged', len(comments) == 1
          and body == capture.get('beforeComment'))
    check('Editor reopened JSON equals persisted JSON; compressed_text is unused', len(saved) == 1
          and saved[0]['compressed_text'] is None and saved[0]['document'] == capture.get('reopened')
          and capture.get('afterComment') == capture.get('reopened'))
    prompt = (REPOSITORY / 'resources/session-summary-prompt.md').read_text().strip()
    auxiliary = [item for item in rounds if prompt in system_text(api, item['request'])]
    check('Auxiliary request uses production summary prompt with all saved source messages, no tools or catalogue',
          len(auxiliary) == 1 and all(text in dump(auxiliary[0]['request']) for text in capture['source'])
          and not auxiliary[0]['request'].get('tools')
          and 'Summary catalogue JSON data:' not in system_text(api, auxiliary[0]['request']))
    capture['generatedMarkdown'] = generated_markdown(api, auxiliary[0]['raw']) if auxiliary else ''
    check('Generated Markdown has both nonempty chapters', bool(re.search(
        r'^# Short summary\s+\S[\s\S]+^# Long summary\s+\S', capture['generatedMarkdown'], re.M)))
    check('Configured Notes provider used', any('provider_id=live_notes' in line for line in capture.get('providerStarts', [])))
    note_id = saved[0]['id'] if len(saved) == 1 else None
    scenarios = ['natural'] if capture.get('natural') else (['direct', 'search'] if lane.get('primary') else ['search'])
    for scenario in scenarios:
        answer = next((item for item in capture.get('answers', []) if item['scenario'] == scenario), None)
        matching = [item for item in rounds if answer and answer['prompt'] in dump(item['request'])]
        first = matching[0]['request'] if matching else {}
        context = system_text(api, first)
        catalogue = None
        if 'Summary catalogue JSON data:\n' in context:
            try:
                catalogue = json.JSONDecoder().raw_decode(context.split('Summary catalogue JSON data:\n', 1)[1])[0]
            except json.JSONDecodeError:
                pass
        items = catalogue.get('items', []) if catalogue else []
        check(scenario + ': catalogue contains exactly the saved short summary once, without code or source transcript',
              len(items) == 1 and items[0].get('note_id') == note_id
              and bool(items[0].get('short_summary'))
              and context.count(dump(items[0]['short_summary'])[1:-1]) == 1
              and context.count('Summary catalogue JSON data:') == 1 and capture['code'] not in dump(first)
              and all(text not in dump(first) for text in capture['source']))
        declarations = tool_declarations(api, first)
        tools = {item.get('name'): item for item in declarations}
        check(scenario + ': both unmodified production summary tool schemas sent with web disabled',
              set(tools) == {'summary_search', 'summary_read'}
              and 'maxLength' in dump(tools) and 'maximum' in dump(tools)
              and 'minimum' in dump(tools) and 'additionalProperties' in dump(tools)
              and '["integer", "null"]' in dump(tools))
        verified, found = verify_continuations(api, matching, scenario, note_id, capture['tag'], capture['code'])
        check(scenario + ': real calls and matching attributed results delivered in order (' + ', '.join(found) + ')', verified)
        check(scenario + ': provider accepted continuation and completed answer displayed',
              bool(answer and answer.get('answer') and matching and matching[-1]['raw']) and verified)
        if scenario == 'natural':
            check('natural: user prompt names neither tool and supplies neither note ID nor code',
                  bool(answer) and all(name not in answer['prompt'] for name in ('summary_search', 'summary_read'))
                  and capture['code'] not in answer['prompt']
                  and str(note_id) not in answer['prompt'].split())
            check('natural: API does not force a tool call', bool(matching) and first.get('tool_choice') in (None, 'auto'))
            check('natural: search result identifies the note without revealing the code', 'summary_search' in found)
            check('natural: displayed answer contains the exact code from the saved comment',
                  bool(answer) and capture['code'] in answer.get('answer', ''))
    if api == 'google':
        signatures = []
        for index, item in enumerate(rounds):
            for obj in response_objects(item['raw']):
                for candidate in obj.get('candidates', []):
                    for part in candidate.get('content', {}).get('parts', []):
                        if part.get('thoughtSignature') and 'functionCall' in part:
                            signatures.append((index, part))
        check('Google thought signatures preserved when supplied on function calls', all(
            any(part in message.get('parts', []) for later in rounds[index + 1:]
                for message in later['request'].get('contents', [])) for index, part in signatures))
        capture['googleCallThoughtSignatures'] = len(signatures)
    return checks


def run_lane(lane, key, output, run_id, keys, timeout, natural=False):
    capture = dict(lane=lane['name'], model=lane['model'], api=lane['api'], stream=lane['stream'], natural=natural)
    logs = []
    try:
        capture['modelVerification'] = verify_model(lane, key)
        with tempfile.TemporaryDirectory(prefix='cha-summary-live-') as directory:
            root = Path(directory)
            process = None
            try:
                capture['settings'] = configure(root, lane, key)
                code = secrets.token_hex(8)
                tag = 'nightjar-' + run_id + '-' + lane['name']
                title = 'Kestrel ' + run_id + '-' + lane['name']
                source = [
                    'For our synthetic Kestrel launch, I initially propose 80 units at West Annex.',
                    'Correction: the accepted decision is 47 units at East Annex. Morgan Vale owns the launch.',
                    'The safety review remains unresolved. The distinctive project tag is kestrel-amber-' + run_id + '.',
                ]
                # Keep the answer beyond the 512-byte search excerpt, so finding
                # the tag alone cannot answer the natural-language question.
                context = (
                    'This follow-up records the coordination notes for the synthetic launch. '
                    'The team will keep a written handover, check the materials against the agreed plan, '
                    'and record any open questions before the next meeting. The record separates proposals '
                    'from accepted decisions and leaves unresolved questions open. Morgan will coordinate '
                    'the handover with the receiving team. These coordination notes describe the planned '
                    'process; they do not settle the outstanding safety review or change the accepted '
                    'launch quantity or location. The acknowledgement belongs to this follow-up record '
                    'and must be copied exactly when reported. The launch team will keep the original '
                    'discussion available so that later questions can be checked against its stated '
                    'decisions. The final reference appears at the end of this comment. '
                ) if natural else ''
                fixture = dict(title=title, tag=tag, code=code, source=source,
                               comment='Follow-up tag: ' + tag + '. ' + context + 'Acknowledgement code: ' + code + '.',
                               primary=bool(lane.get('primary')), natural=natural)
                capture.update(fixture)
                script = root / 'summary-live.js'
                script.write_text('const summaryLive = ' + dump(fixture) + ';\n'
                                  + (REPOSITORY / 'tests/native/summary-live.js').read_text())
                environment = dict(os.environ, CHA_NATIVE_TEST_CONFIG=str(root),
                                   CHA_NATIVE_TEST_HOST=str(root / 'SummaryLiveHost.app'),
                                   CHA_NATIVE_PARITY_SCRIPT=str(script), CHA_NATIVE_ASSETS=str(REPOSITORY / 'webapp/dist'))
                process = subprocess.Popen(['tests/native/macos/run.sh', 'parity', '--timeout-ms', str(timeout * 1000)],
                                           cwd=REPOSITORY, env=environment, stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE, text=True, start_new_session=True)
                try:
                    stdout, stderr = process.communicate(timeout=timeout + 60)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGTERM)
                    stdout, stderr = process.communicate(timeout=10)
                    capture['reason'] = 'Native host exceeded lane timeout'
                capture['hostExit'] = process.returncode
                capture['hostStderr'] = stderr
                if process.returncode:
                    capture.setdefault('reason', stderr.strip() or 'Native host failed without a report')
                for line in stdout.splitlines():
                    try:
                        result = json.loads(line)
                        if isinstance(result, dict) and 'ok' in result:
                            capture.update(result)
                    except json.JSONDecodeError:
                        pass
                capture['saved'] = documents(root)
            finally:
                if process and process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.communicate()
                # Rotations: largest suffix is oldest; active runtime.log is newest.
                paths = sorted(root.glob('runtime.*.log'),
                               key=lambda path: int(path.stem.split('.')[-1]), reverse=True)
                if (root / 'runtime.log').exists():
                    paths.append(root / 'runtime.log')
                for path in paths:
                    name = lane['name'] + '-' + path.name
                    content = redact(path.read_text(errors='replace'), keys)
                    (output / name).write_text(content)
                    logs.append((name, content))
                # TemporaryDirectory removes the DB, WAL/SHM, key config, and this host only.
    except Exception as error:
        capture['reason'] = redact(type(error).__name__ + ': ' + str(error), keys)
    capture['logs'] = [name for name, _ in logs]
    capture['endpoints'] = list(dict.fromkeys(match.group(1) for _, content in logs
        for match in re.finditer(r'HTTP request started: endpoint=([^ ]+)', content)))
    capture['providerStarts'] = [line for _, content in logs for line in content.splitlines()
                                 if 'Provider request started:' in line or 'Model round started:' in line]
    try:
        rounds = read_rounds(logs)
        capture['requestOrder'] = [dict(order=item['order'], log=item['log'],
                                       model=item['request'].get('model', lane['model']),
                                       purpose=next((answer['scenario'] for answer in capture.get('answers', [])
                                           if answer['prompt'] in dump(item['request'])), 'summary')) for item in rounds]
        capture['checks'] = inspect(lane, capture, rounds) if capture.get('source') else []
    except Exception as error:
        capture['checks'] = [dict(check='Evidence inspection: ' + str(error), passed=False)]
    capture['demonstrated'] = bool(capture.get('checks')) and all(item['passed'] for item in capture['checks'])
    capture['diagnostics'] = [line for _, content in logs for line in content.splitlines()
                              if ' data=' not in line and any(marker in line for marker in ('[warning]', '[error]', 'retry', 'incomplete',
                                                                  'HTTP transfer finished:', 'Model round finished:'))]
    return capture


def write_report(output, run_id, captures, keys):
    (output / 'capture.json').write_text(redact(json.dumps(captures, ensure_ascii=False, indent=2), keys) + '\n')
    lines = ['# Summary live experiment ' + run_id, '',
             'Synthetic UI conversations; production frontend, converter, bridge, runtime and HTTP clients.',
             'Prerequisites: cmake --preset ninja; build cha_macos_runtime and cha_prepare_test_vault; npm --prefix webapp run build.',
             'No tool schemas were changed. Account model lists were checked before each lane. No vaults retained.', '']
    for capture in captures:
        lines += ['## ' + capture['lane'], '', 'Model: `' + capture['model'] + '`; API: `'
                  + capture['api'] + '`; streaming: ' + str(capture['stream']).lower() + '.', '']
        if capture.get('skipped'):
            lines += [capture['reason'], '']
            continue
        lines += ['Demonstrated: ' + ('yes' if capture['demonstrated'] else 'no') + '.',
                  'Consumer scenario: ' + ('natural question, with no tool names' if capture.get('natural') else 'directed retrieval') + '.',
                  'Endpoint: ' + ', '.join('`' + endpoint + '`' for endpoint in capture.get('endpoints', [])) + '.',
                  'Settings: ' + dump(capture.get('settings', {})) + '.', '']
        if capture.get('reason'):
            lines += ['Observed error or unexercised step: ' + capture['reason'], '']
        for check in capture.get('checks', []):
            lines.append('- [' + ('x' if check['passed'] else ' ') + '] ' + check['check'])
        lines += ['', 'Evidence: `capture.json` and ' + ', '.join('`' + name + '`' for name in capture.get('logs', []))
                  + '; request order is listed in capture.json. Raw JSON/SSE and diagnostics are retained in the logs.', '']
        markdown = capture.get('generatedMarkdown', '')
        if not markdown:
            lines += ['No summary returned; model-quality observations are unavailable.', '']
            continue
        lines += ['Model observations (separate from delivery checks):', '',
                  '- Accepted correction (47 / East Annex) present: ' + str('47' in markdown and 'East Annex' in markdown) + '.',
                  '- Owner and unresolved review present: ' + str('Morgan Vale' in markdown and 'safety' in markdown.lower()) + '.',
                  '- Keywords line present: ' + str('Keywords:' in markdown) + '.']
        for answer in capture.get('answers', []):
            lines.append('- ' + answer['scenario'] + ' answer repeats exact saved code: '
                         + str(capture['code'] in answer.get('answer', '')) + '.')
        lines += ['', 'Effective submitted settings, incomplete responses, transfer status and retries: see request bodies and diagnostics in capture.json.', '']
    (output / 'report.md').write_text(redact('\n'.join(lines), keys))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lane', action='append', choices=[lane['name'] for lane in LANES])
    parser.add_argument('--natural', action='store_true', help='Run only the autonomous search/read scenario after saving the fixture')
    parser.add_argument('--timeout', type=int, default=600, help='Native host timeout per lane, seconds')
    args = parser.parse_args()
    os.umask(0o077)
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + secrets.token_hex(3)
    output = REPOSITORY / 'work/summary-live' / run_id
    output.mkdir(parents=True)
    keys = [os.environ.get(lane['key'], '') for lane in LANES] + [os.environ.get('OPEN_ROUTER_API_KEY', '')]
    captures = []
    for lane in LANES:
        if args.lane and lane['name'] not in args.lane:
            continue
        key = os.environ.get(lane['key'])
        if not key and lane['name'] == 'openrouter':
            key = os.environ.get('OPEN_ROUTER_API_KEY')
        if not key:
            capture = dict(lane=lane['name'], model=lane['model'], api=lane['api'], stream=lane['stream'],
                           skipped=True, demonstrated=False, reason='Not run: missing ' + lane['key'] + '.')
        else:
            print(lane['name'] + ': starting ' + lane['model'], flush=True)
            capture = run_lane(lane, key, output, run_id, keys, args.timeout, natural=args.natural)
        captures.append(capture)
        write_report(output, run_id, captures, keys)
        print(lane['name'] + ': ' + ('demonstrated' if capture['demonstrated'] else capture.get('reason', 'coverage incomplete')), flush=True)
    print('Report: ' + str(output / 'report.md'), flush=True)
    return 0 if all(item['demonstrated'] or item.get('skipped') for item in captures) else 1


if __name__ == '__main__':
    raise SystemExit(main())
