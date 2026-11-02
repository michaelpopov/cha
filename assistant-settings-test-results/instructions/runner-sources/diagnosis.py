import subprocess,sys,pathlib
r=pathlib.Path(__file__).resolve().parent
for n in ['W15-diagnosis','W16-diagnosis']:
 p=subprocess.run([sys.executable,str(r/'driver.py'),n])
 if p.returncode:break
