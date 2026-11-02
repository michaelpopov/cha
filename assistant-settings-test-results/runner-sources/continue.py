import subprocess,sys,pathlib
r=pathlib.Path(__file__).resolve().parent
for name in ['W08-continue','W10-continue','W15-continue','W16-continue']:
 subprocess.run([sys.executable,str(r/'run.py'),name])
