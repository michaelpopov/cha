import subprocess,sys,pathlib
r=pathlib.Path(__file__).resolve().parent
steps=[['fixture','restore-assistant'],['run','F-after-import-fix'],['fixture','model','gpt-4o'],['run','gpt4o-F05-after-import-fix'],['fixture','model','gpt-6.1-sol'],['run','W12-after-import-fix'],['run','W16-after-import-fix'],['fixture','gem-reset'],['run','W18-isolated']]
for s in steps:
 cmd=[sys.executable,str(r/'driver.py'),s[1]]if s[0]=='run'else[str(r/'fixture'),*s[1:]]
 p=subprocess.run(cmd);print(s,'exit',p.returncode,flush=True)
 if p.returncode:break
