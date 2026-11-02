import os,json,pathlib,re,shlex,urllib.request,urllib.error
repo=pathlib.Path('/Users/mpopov/projects/cha');out=repo/'assistant-settings-test-results/instructions';profile=(pathlib.Path.home()/'.zprofile').read_text();keys={}
for name in ['OPENAI_API_KEY','GEMINI_API_KEY']:
 value=os.environ.get(name)
 if not value:
  m=re.search(r'(?m)^\s*(?:export\s+)?'+name+r'=(.*)$',profile);value=shlex.split(m.group(1))[0] if m else None
 keys[name]=value
checks=[]
for row in json.loads((out/'remote-resources.json').read_text()):
 requests=[]
 if row['service']=='openai':
  requests.append(('https://api.openai.com/v1/vector_stores/'+row['remote_id'],{'Authorization':'Bearer '+keys['OPENAI_API_KEY']}))
  requests.extend(('https://api.openai.com/v1/files/'+f,{'Authorization':'Bearer '+keys['OPENAI_API_KEY']}) for f in row['files'])
 else:requests.append(('https://generativelanguage.googleapis.com/v1beta/'+row['remote_id'],{'x-goog-api-key':keys['GEMINI_API_KEY']}))
 for url,headers in requests:
  try:
   with urllib.request.urlopen(urllib.request.Request(url,headers=headers),timeout=30) as response:status=response.status
  except urllib.error.HTTPError as e:status=e.code
  checks.append({'url':url,'http_status':status,'removed':status==404})
(out/'remote-cleanup-verification.json').write_text(json.dumps(checks,indent=2));print(json.dumps(checks,indent=2));assert all(c['removed'] for c in checks)
