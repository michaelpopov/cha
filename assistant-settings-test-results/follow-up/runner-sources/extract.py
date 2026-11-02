from pathlib import Path
import json,gzip,re,hashlib,datetime,zoneinfo
R=Path('/Users/mpopov/projects/cha/assistant-settings-test-results/follow-up')
for d in R.iterdir():
 if not d.is_dir()or not(d/'debug.log.gz').exists():continue
 logs=gzip.decompress((d/'debug.log.gz').read_bytes()).decode();requests=[];calls={};native_count=len(re.findall(r'\[info\] Maintenance tool call: name=',logs));responses=[]
 for line in logs.splitlines():
  if 'HTTP model request body data=' in line:
   try:b=json.loads(json.loads(line.split(' data=',1)[1]))
   except:continue
   s=b.get('instructions','');prompt=next((x.get('content','')for x in reversed(b.get('input',[]))if x.get('role')=='user'),'');utc=datetime.datetime.strptime(line[1:27],'%Y-%m-%d %H:%M:%S.%f').replace(tzinfo=zoneinfo.ZoneInfo('America/Los_Angeles')).astimezone(datetime.timezone.utc).isoformat()
   requests.append({'utc':utc,'model':b.get('model'),'prompt':prompt,'tools':[t.get('name',t.get('type'))for t in b.get('tools',[])],'instructions_sha256':hashlib.sha256(s.encode()).hexdigest(),'guide_present':'# CHA application guide'in s,'fixed_file_navigation':'Character settings → Files → New file'in s,'maintenance_present':'# Operating instructions for Assistant'in s})
   inputs=b.get('input',[])
   for i,x in enumerate(inputs):
    if x.get('type')!='function_call':continue
    id=x['call_id']
    if id not in calls:calls[id]={'call_id':id,'utc':utc,'prompt':next((m.get('content','')for m in reversed(inputs[:i])if m.get('role')=='user'),''),'name':x['name'],'arguments':json.loads(x['arguments'])}
    result=next((m for m in inputs if m.get('type')=='function_call_output'and m.get('call_id')==id),None)
    if result:
     try:calls[id]['result']=json.loads(result['output'])
     except:calls[id]['result']=result['output']
  if 'HTTP model raw response data='in line:
   try:raw=json.loads(line.split(' data=',1)[1])
   except:continue
   for part in raw.splitlines():
    if not part.startswith('data: '):continue
    try:v=json.loads(part[6:])
    except:continue
    if v.get('type')=='response.completed':responses.append(v.get('response',{}))
 (d/'requests.json').write_text(json.dumps(requests,indent=2));(d/'tool-calls.json').write_text(json.dumps(list(calls.values()),indent=2))
 maint=[c for c in calls.values()if c['name'].startswith(('vault_config_','host_config_','assistant_'))or c['name']=='add_character'];incomplete=[c['call_id']for c in maint if'result'not in c]
 (d/'evidence-check.json').write_text(json.dumps({'native_maintenance_call_count':native_count,'captured_maintenance_call_count':len(maint),'missing_results':incomplete,'tool_results_complete':native_count==len(maint)and not incomplete,'completed_model_responses':len(responses),'models':sorted({r['model']for r in requests if r['model']})},indent=2))
print('Extracted completed per-case logs.')
