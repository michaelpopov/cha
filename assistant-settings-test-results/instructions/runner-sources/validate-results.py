from pathlib import Path
import json,hashlib,difflib,tomllib,xml.etree.ElementTree as ET,datetime
R=Path('/Users/mpopov/projects/cha');O=R/'assistant-settings-test-results/instructions'
def read(p):return json.loads(p.read_text())
def data(n):return read(O/n/'observations.json')
def calls(n):return read(O/n/'tool-calls.json')
def exports(n,when):return read(O/n/(when+'-export.json'))
def changes(n):
 a,b=exports(n,'before'),exports(n,'after');return [k for k in sorted(a.keys()|b.keys())if a.get(k)!=b.get(k)]
def applies(n):return [c for c in calls(n)if c['name']=='vault_config_apply']
def answer(n,key):return [t['text']for t in data(n)['C'][key]['transcript']if t['kind']=='character'][-1]
checks=[]
def check(name,v):
 checks.append({'check':name,'passed':bool(v)});assert v,name
mapping={'W-01':'W01','W-02':'W02-W03','W-03':'W02-W03','W-04':'W04','W-05':'W05','W-07':'W07-W18','W-09':'W09','W-11':'W11','W-12':'W12','W-14':'W14','W-15':'W15-diagnosis','W-16':'W16-final','F-01':'F','F-04':'F04-final'}
for n in sorted(set(mapping.values())):
 o=data(n);e=read(O/n/'evidence-check.json');check(n+': driver, checkpoint exports and complete native tool results',o['ok']and not o['checkpoint_errors']and e['tool_results_complete'])
for n in ['W01','W05','W09','W11','W12','W15-diagnosis','W16-final','F','F04-final']:
 check(n+': no configuration changes',changes(n)==[])
check('W02: read-only before authorization',exports('W02-W03','W02-before')==exports('W02-W03','W02-after'))
w3=applies('W02-W03');check('W03: exactly one create, committed, undo cleared',len(w3)==1 and w3[0]['arguments']['changes'][0]['operation']=='create' and w3[0]['result']['committed']and not w3[0]['result']['undo_available'])
c=data('W02-W03')['C'];check('W03: real compression with original note retained and success Event',bool(c['compressed']['compressed_text'])and c['compressed']['document']['content'][0]['content'][0]['text']=='a'*4300 and any(e['severity']=='success'for e in c['events']['items']))
w4=applies('W04');check('W04: High then undo then unavailable; exact bytes restored',len(w4)==3 and w4[0]['result']['committed']and w4[1]['result']['committed']and w4[2]['result']['error']=='unavailable_undo'and exports('W04','before')==exports('W04','after'))
check('W04: native settings Low restored and no silent forward edit',data('W04')['C']['step1Settings']['compression_reasoning_effort']=='high' and data('W04')['C']['step2Settings']['compression_reasoning_effort']=='low' and data('W04')['C']['step3Settings']['compression_reasoning_effort']=='low')
w5=applies('W05');check('W05: one protected apply rejected',len(w5)==1 and not w5[0]['result']['committed']and w5[0]['result']['error']=='credential_destination_protected')
b7=exports('W07-W18','after');check('W07: ignored High and Google Notes xhigh were actually saved',tomllib.loads(b7['characters/character_2/character.toml'])['reasoning_effort']=='high'and tomllib.loads(b7['system/notes/config.toml'])['compression_provider']=='provider_3' and tomllib.loads(b7['system/notes/config.toml'])['compression_reasoning_effort']=='xhigh')
check('W09: no writes and actual character/provider read',not applies('W09')and {'characters/character_3/character.toml','system/providers/provider_4/config.toml'}<=set(p for c in calls('W09')for p in c['arguments'].get('paths',[])))
check('W09: real Anthropic reproduction prerequisite missing',not data('W09')['C']['anthropic_credential'])
check('W11: all three fixture store uploads succeeded',all(v['result']=='success'for v in data('upload')['C']['stores'].values()))
huge=[c for c in calls('W12')if c['name']=='assistant_logs'and c['arguments'].get('contains')=='##000005'];check('W12: actual Huge note omission found',any('exceeds 256 KiB' in json.dumps(c['result'])for c in huge))
a,b=exports('W14','before'),exports('W14','after');p='forums/forum_1/config.toml';before,after=tomllib.loads(a[p]),tomllib.loads(b[p]);expected=dict(before);expected['file_references']=[x for x in before['file_references']if x['ref_id']!='##00000A'];check('W14: only requested forum reference removed',changes('W14')==[p]and after==expected and len(applies('W14'))==1)
s=data('W15-diagnosis')['C']['sessionSettings'];sc=next(x for x in s['chat_settings']['characters']if x['character_id']=='character_1');check('W15: native session override Google while saved provider OpenAI',sc['provider_id']=='provider_3'and tomllib.loads(exports('W15-diagnosis','before')['characters/character_1/character.toml'])['provider']=='provider_2')
w16=calls('W16-final');check('W16: Jev missing and Notes read before current diagnosis',any({'system/jev/config.toml','system/notes/config.toml'}<=set(c['arguments'].get('paths',[]))and any(f['path']=='system/jev/config.toml'and f['status']=='missing'for f in c['result']['files'])for c in w16 if c['name']=='vault_config_read'))
check('W16: no writes, no false retraction on next turn',not applies('W16-final')and 'not backed by tool inspection'not in answer('W16-final','step2'))
for n in ['F','F04-final']:
 req=read(O/n/'requests.json');check(n+': maintenance tools unavailable outside Welcome',not calls(n)and all(not x['maintenance_present']and not any(t.startswith('vault_config_')for t in x['tools'])for x in req))
check('F04: corrected Files restart explanation present','does **not** restart' in answer('F04-final','answer'))
for exe in ['cha_tests','cha_app_tests','cha_bridge_tests','cha_native_runtime_tests']:
 root=ET.parse(O/(exe+'.xml')).getroot();check(exe+': native suite no failures',int(root.get('failures',0))==0 and int(root.get('errors',0))==0)
for label in ['prompt-location-final','prompt-assembly-final']:
 root=ET.parse(O/(label+'.xml')).getroot();check(label+': final prompt integration check',int(root.get('failures',0))==0 and int(root.get('errors',0))==0)
prior=read(O/'before-source-hashes.json');final={p:hashlib.sha256((R/p).read_bytes()).hexdigest()for p in prior};check('Only prompt resources changed in this task; previous application fixes retained',all(final[p]==h for p,h in prior.items()if not p.startswith('resources/')));(O/'final-source-hashes.json').write_text(json.dumps(final,indent=2))
patch=[]
for p in ['resources/application-guide.md','resources/assistant-maintenance.md']:
 patch.extend(difflib.unified_diff((O/(Path(p).name+'.before')).read_text().splitlines(True),(R/p).read_text().splitlines(True),fromfile='before/'+p,tofile='after/'+p))
(O/'instruction-changes.patch').write_text(''.join(patch));(O/'validation-checks.json').write_text(json.dumps(checks,indent=2))
notes={
'W-01':('Read current Notes, provider and key metadata; explains all four fields, Medium vs High default, desktop audio, imports and summaries.','No writes; saved audio URL appears in raw answer but the established global URL filter removes it from the rendered table.'),
'W-02':('Read missing Notes and available providers; proposes actual provider_2/openai-direct and correct manual screen.','Fresh fixture, no Events contamination; real missing-provider error reproduced before question; no write before authorization.'),
'W-03':('One authorized create with openai-direct and Low; answer reports path, validation, undo clearing and verification boundary.','Real compression succeeds afterward; note document intact and success Event recorded.'),
'W-04':('High saved, undo restores Low; second undo unavailable and offers an unperformed forward edit.','Exact baseline export restored; no silent forward repair.'),
'W-05':('Single protected audio-destination attempt refused; exact desktop Notes settings recovery.','No configuration change; no credential request or bypass.'),
'W-07':('Correctly saves explicitly requested High and explains ignored Google effort; Notes provider change with xhigh also saved correctly.','Strict Fail: step 1 demands extra informed confirmation. This conflicts with established instructions authorizing explicit changes; no extra approval machinery added.'),
'W-09':('Reads actual Claude Test/provider; correctly explains model-default thinking and ignored effort plus temperature.','Knowledge answer Pass. Full live reproduction Blocked: ANTHROPIC_API_KEY absent; placeholder key used only for the read-only explanation, never as a successful Anthropic call.'),
'W-11':('Explains actual scopes, same-key native routes, cross-provider function route, two OpenAI native stores, Gemini 2.5 restriction and Welcome exclusion.','Real stores uploaded. Answer correctly leaves store readiness/key matching conditional because configuration tools cannot inspect those details.'),
'W-12':('Checks Huge note Ref ID log for actual 256 KiB omission; first-use compressed copies and both refresh methods explained.','Long note inclusion is not asserted without proof; no writes.'),
'W-14':('Full Files navigation and validation rationale; authorized removal deletes only Docs Removed reference.','One forum replacement preserves all other configuration; undo available.'),
'W-15':('Reads current Writer and provider, states saved values, explains both session-only Model and Effort and Default.','Native S1 snapshot independently confirms Google override while global provider remains OpenAI; no writes.'),
'W-16':('Reads missing Jev and actual Notes; explains detection, compression-provider summaries, sidebar fallback, history distinction, and Welcome refusal.','Final next-turn reply no longer falsely retracts real tool inspection; no writes.'),
'F-01':('Ordinary-forum answer gives exact Notes settings navigation, provider plus effort, shared import and summary uses.','No maintenance tools or writes. In F evidence, C.caseIds maps answer 0/checkpoint F01 to F-01.'),
'F-04':('Full upload, Copy Ref ID, Files/New file/Folder reference workflow and detach scope.','Final reply correctly says Files references do not restart chats; no maintenance tools or writes. Earlier F answer 1/checkpoint F02 is F-04, not spec F-02.')}
rows=[]
for id,n in mapping.items():
 verdict='Fail'if id=='W-07'else 'Blocked'if id=='W-09'else 'Pass'
 rows.append({'id':id,'previous_verdict':'Fail','verdict':verdict,'evidence_directory':n,'result':notes[id][0],'validation_and_limits':notes[id][1]})
(O/'results.json').write_text(json.dumps({'scope':'The 14 previously failing live cases; latest applicable run for each case, not a claim of model determinism.','summary':{'pass':12,'fail':1,'blocked':1,'native_pass':1632,'native_skip':2,'final_prompt_checks_pass':2},'cases':rows,'validation_checks':len(checks)},indent=2))
text='Assistant instruction follow-up — validated results\n\n12 Pass; 1 strict-spec Fail (W-07); 1 Blocked reproduction (W-09, knowledge answer Pass).\n\n'
text+='Changed resources/application-guide.md and resources/assistant-maintenance.md. The changes clarify current-state inspection, saved/effective/default settings, session overrides, Notes dependencies and shared uses, committed-save and undo reports, manual credential recovery, and Files navigation/routing/limits. Earlier application fixes are retained without further code changes.\n\n'
for row in rows:text+=f"{row['id']} {row['verdict']} — {row['result']} {row['validation_and_limits']} Evidence: {row['evidence_directory']}/\n\n"
text+='Revision history: initial instruction run preserved in each original case directory. W15 and W16 initially still skipped current reads; diagnosis rule added and rerun as W15-diagnosis and W16-diagnosis. W16-diagnosis then falsely retracted its actual earlier tool inspection; temporal-context clarification added and W16-final passed. Initial F-04 had the required steps but an overbroad restart warning; guide now distinguishes Character settings form save from Files changes, and F04-final passed. These are changes with targeted reruns, not discarded retries. Final requests retain embedded-prompt SHA-256 per request. Other cases were not repeated after these narrow final clarifications.\n\n'
text+='Scope note: W18 was exercised only as a continuation of W07. Since High was already saved, its confirmation was a no-op; this run does not demonstrate the committed-warning branch of W18 and does not regrade that separate case.\n\n'
text+='Native validation: 1,632 passed and 2 allowed OAuth skips across all four suites after initial instruction changes. Final resource build and both prompt assembly/tool-location checks pass after the last revision. File-reference no-restart behavior has existing bridge coverage in FileReferencesValidateRequestsAndLeaveLiveSessionsUnchanged.\n\n'
text+='Validation: complete per-case native maintenance call counts match captured call/results; checkpoint exports prove authorized write boundaries; no maintenance tools outside Welcome. Validation script assertions are in validation-checks.json. Live answers are judged against spec behavior and independently checked native evidence, rather than driver exit status alone.\n\n'
text+='Additional observation outside the graded Welcome replies: the ordinary-forum Assistant in the W16 precondition wrongly called Lab Entrance/Welcome. Native snapshot proves it was Lab and no summary was created. This is a remaining model context mistake, not a save success, and is retained as a limitation rather than concealed by the passing Welcome diagnosis. It was not part of the 14 strict expected answer checks.\n\n'
text+='Cleanup: normal Remove from store completed for the two remaining owned stores; F3 was removed during setup. Read-only provider GETs return 404 for all three stores and both OpenAI files. The isolated vault, sidecars, session directory and exact registration are deleted. Original host app.toml restored byte for byte; raw secret-bearing exports removed immediately, retained exports redacted. See remote-cleanup-verification.json and local-cleanup-verification.json.\n'
(O/'results.txt').write_text(text)
print('Validated',len(checks),'evidence assertions; wrote per-case results and revision history.')
