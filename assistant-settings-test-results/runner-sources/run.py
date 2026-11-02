from pathlib import Path
import os,json,subprocess,time,hashlib,datetime,difflib,sys,shutil
R=Path(__file__).resolve().parent;REPO=Path('/Users/mpopov/projects/cha');OUT=REPO/'assistant-settings-test-results';HOST=Path.home()/'Library/Application Support/CHA';APP=HOST/'app.toml';C=json.loads((R/'fixture.json').read_text());C['notes']={}
OUT.mkdir(exist_ok=True)
def export(label):
 p=R/'exports'/label;p.mkdir(parents=True,exist_ok=False);subprocess.run([str(R/'fixture'),'export',str(p)],check=True,stdout=subprocess.DEVNULL)
 # Keep a redacted independent export record, and delete the secret-bearing export immediately.
 result={}
 for f in sorted(p.rglob('*')):
  if f.is_file():
   n=str(f.relative_to(p));text=f.read_text()
   if n.startswith('system/keys/'):
    import re
    text=re.sub(r'(?m)^(value|secret_key|access_key_id)\s*=.*$',r'\1 = "[REDACTED]"',text)
   result[n]=text
 shutil.rmtree(p);(OUT/(label+'-export.json')).write_text(json.dumps(result,indent=2));return result
name=sys.argv[1];code=(R/(name+'.js')).read_text();before=export(name+'-before');script=R/'active.js';script.write_text('let C='+json.dumps(C)+';\n'+(R/'common.js').read_text()+'\nasync function phase(){\n'+code+'\n}\n')
original=APP.read_bytes();(R/'original-app.toml').write_bytes(original)
try:
 APP.write_text("vault = 'Assistant-Capability-Test-20261101'\n[logging]\nfile = '/private/tmp/cha-ass-settings-run/runtime.log'\nlevel = 'debug'\n")
 env=dict(os.environ,CHA_NATIVE_PARITY_SCRIPT=str(script),CHA_NATIVE_SCREENSHOT_DIR=str(OUT/'screenshots'))
 start=datetime.datetime.now(datetime.timezone.utc).isoformat();print('Starting',name,flush=True)
 p=subprocess.run([str(R/'AssistantTestHost.app/Contents/MacOS/ChaNativeTestHost'),'--assets',str(REPO/'webapp/dist'),'--config',str(HOST),'--expect','parity','--timeout-ms','1200000'],env=env,stdout=(R/(name+'.stdout')).open('w'),stderr=(R/(name+'.stderr')).open('w'),timeout=1250)
 raw=(R/(name+'.stdout')).read_text();j=json.loads(raw);j.update(exit_code=p.returncode,utc=start);(OUT/(name+'.json')).write_text(json.dumps(j,indent=2));print(name,'exit',p.returncode,'ok',j.get('ok'),j.get('reason',''),flush=True)
finally:
 APP.write_bytes(original)
after=export(name+'-after');diff=[]
for f in sorted(before.keys()|after.keys()):
 if before.get(f)!=after.get(f):diff.extend(difflib.unified_diff(before.get(f,'').splitlines(True),after.get(f,'').splitlines(True),fromfile='before/'+f,tofile='after/'+f))
(OUT/(name+'.diff')).write_text(''.join(diff) or 'no change\n')
print('Changed paths',[f for f in sorted(before.keys()|after.keys()) if before.get(f)!=after.get(f)],flush=True)
