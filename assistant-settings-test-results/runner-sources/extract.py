import re,json,pathlib,sys
log=pathlib.Path('/private/tmp/cha-ass-settings-run/runtime.log');out=pathlib.Path('/Users/mpopov/projects/cha/assistant-settings-test-results')
records=[]
for line in '\n'.join(p.read_text() for p in sorted(log.parent.joinpath('archived-debug').glob('runtime*.log'))).splitlines():
 if 'HTTP model request body data=' in line:
  try:body=json.loads(json.loads(line.split(' data=',1)[1]))
  except Exception:continue
  records.append({'time':line[1:27],'body':body})
seen=set();calls=[]
for r in records:
 for i in r['body'].get('input',[]):
  if i.get('type')=='function_call' and i['call_id'] not in seen:
   seen.add(i['call_id']);x={'utc_log_time':r['time'],'name':i['name'],'arguments':json.loads(i['arguments']),'call_id':i['call_id'],'prompt':next((m.get('content','') for m in reversed(r['body']['input'][:r['body']['input'].index(i)]) if m.get('role')=='user'),'')}
   result=next((x for x in r['body'].get('input',[]) if x.get('type')=='function_call_output' and x.get('call_id')==i['call_id']),None)
   if result:
    try:x['result']=json.loads(result['output'])
    except:x['result']=result['output']
   calls.append(x)
(out/'live-tool-calls.json').write_text(json.dumps(calls,indent=2))
print(json.dumps([c for c in calls if c['name']=='vault_config_apply'],indent=2))
