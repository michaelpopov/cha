import subprocess,sys,pathlib
r=pathlib.Path(__file__).resolve().parent
for n in ['W16-final','F04-final']:
 p=subprocess.run([sys.executable,str(r/'driver.py'),n])
 if p.returncode:break
