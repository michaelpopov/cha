from pathlib import Path
import os,json,subprocess,time,hashlib,datetime,difflib,sys,shutil,gzip,threading
R=Path(__file__).resolve().parent;REPO=Path('/Users/mpopov/projects/cha');OUT=REPO/'assistant-settings-test-results/instructions';HOST=Path.home()/'Library/Application Support/CHA';APP=HOST/'app.toml';HOSTBIN=Path('/private/tmp/cha-ass-settings-run/AssistantTestHost.app/Contents/MacOS/ChaNativeTestHost')
name=sys.argv[1]
code=(R/(name+'.js')).read_text();C=json.loads((R/'fixture.json').read_text());C['notes']={};C['welcome']={'forum_id':'builtin-entrance','session_id':'builtin-welcome'}
O=OUT/name;O.mkdir(parents=True,exist_ok=False);(R/'checkpoints').mkdir(exist_ok=True);images=R/'checkpoint-images';images.mkdir(exist_ok=True)
for p in images.glob('*.png'):p.unlink()
for p in (R/'checkpoints').glob('*.txt'):p.unlink()
records=[];export_lock=threading.Lock()
def export(label):
 with export_lock:
  p=R/'exports'/name/label;p.mkdir(parents=True,exist_ok=False);subprocess.run([str(R/'fixture'),'export',str(p)],check=True,stdout=subprocess.DEVNULL)
  result={}
  import re
  for f in sorted(p.rglob('*')):
   if f.is_file():
    n=str(f.relative_to(p));text=f.read_text()
    if n.startswith('system/keys/'):text=re.sub(r'(?m)^(value|secret_key|access_key_id)\s*=.*$',r'\1 = "[REDACTED]"',text)
    result[n]=text
  shutil.rmtree(p);(O/(label+'-export.json')).write_text(json.dumps(result,indent=2));return result
before=export('before');script=R/'active.js';script.write_text('let C='+json.dumps(C)+';\n'+(R/'common.js').read_text()+'\nasync function phase(){\n'+code+'\n}\n')
stop=threading.Event();errors=[]
def monitor():
 seen=set()
 while not stop.is_set():
  for p in images.glob('*.png'):
   if p.stem in seen:continue
   seen.add(p.stem)
   try:
    export(p.stem);(R/'checkpoints'/(p.stem+'.txt')).write_text('ready');records.append(p.stem)
   except Exception as e:errors.append(str(e))
  stop.wait(.1)
thread=threading.Thread(target=monitor,daemon=True);thread.start();original=APP.read_bytes()
if not (R/'original-app.toml').exists():(R/'original-app.toml').write_bytes(original)
assert original==(R/'original-app.toml').read_bytes(),'Host settings changed since starting follow-up'
log=R/(name+'-runtime.log');test_app="vault = 'Assistant-Instructions-Test'\n[logging]\nfile = '"+str(log)+"'\nlevel = 'debug'\n"
try:
 runtime=REPO/'build/ninja/libChaRuntime.dylib';bundled=HOSTBIN.parent.parent/'Frameworks/libChaRuntime.dylib'
 if hashlib.sha256(runtime.read_bytes()).digest()!=hashlib.sha256(bundled.read_bytes()).digest():shutil.copyfile(runtime,bundled)
 APP.write_text(test_app);env=dict(os.environ,CHA_NATIVE_PARITY_SCRIPT=str(script),CHA_NATIVE_SCREENSHOT_DIR=str(images));start=datetime.datetime.now(datetime.timezone.utc).isoformat();print('Starting',name,flush=True)
 p=subprocess.run([str(HOSTBIN),'--assets',str(REPO/'webapp/dist'),'--config',str(HOST),'--expect','parity','--timeout-ms','1200000'],env=env,stdout=(R/(name+'.stdout')).open('w'),stderr=(R/(name+'.stderr')).open('w'),timeout=1250)
 j=json.loads((R/(name+'.stdout')).read_text());j.update(exit_code=p.returncode,utc=start,checkpoints=records,checkpoint_errors=errors);(O/'observations.json').write_text(json.dumps(j,indent=2));print(name,'exit',p.returncode,'driver_ok',j.get('ok'),j.get('reason',''),flush=True)
finally:
 stop.set();thread.join(5)
 if APP.read_text()==test_app:APP.write_bytes(original)
 else:raise RuntimeError('Host settings changed during run; preserved concurrent edit')
after=export('after');diff=[]
for f in sorted(before.keys()|after.keys()):
 if before.get(f)!=after.get(f):diff.extend(difflib.unified_diff(before.get(f,'').splitlines(True),after.get(f,'').splitlines(True),fromfile='before/'+f,tofile='after/'+f))
(O/'config.diff').write_text(''.join(diff) or 'no change\n')
logs=sorted(R.glob(name+'-runtime*.log'))
with gzip.open(O/'debug.log.gz','wb') as dest:
 for f in logs:dest.write(f.read_bytes())
(O/'log-files.json').write_text(json.dumps([{'name':p.name,'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in logs],indent=2))
print('Changed paths',[f for f in sorted(before.keys()|after.keys()) if before.get(f)!=after.get(f)],flush=True)
