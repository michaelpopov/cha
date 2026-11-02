import os,re,shlex,pathlib,json,urllib.request
p=(pathlib.Path.home()/'.zprofile').read_text();key=os.environ.get('OPENAI_API_KEY')
if not key:
 m=re.search(r'(?m)^\s*(?:export\s+)?OPENAI_API_KEY=(.*)$',p);key=shlex.split(m.group(1))[0]
with urllib.request.urlopen(urllib.request.Request('https://api.openai.com/v1/models',headers={'Authorization':'Bearer '+key}),timeout=30) as response:
 models=json.load(response)
names=sorted(x['id'] for x in models['data'] if x['id'].startswith(('gpt-5','gpt-6')))
print(json.dumps(names));pathlib.Path('/Users/mpopov/projects/cha/assistant-settings-test-results/follow-up/available-reasoning-models.json').write_text(json.dumps(names,indent=2))
