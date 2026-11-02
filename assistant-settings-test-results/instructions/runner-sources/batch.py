import subprocess,sys,pathlib
r=pathlib.Path(__file__).resolve().parent
steps=[['run','W02-W03'],['fixture','notes','openai','low'],['run','W04'],['fixture','notes','openai','medium','audio'],['run','W01'],['fixture','notes','openai','low'],['run','W05'],['fixture','gem-reset'],['run','W07-W18'],['run','W09'],['run','upload'],['run','W11'],['run','W12'],['run','W15'],['fixture','notes','openai','low'],['run','W16'],['run','F'],['run','W14']]
for step in steps:
 cmd=[sys.executable,str(r/'driver.py'),step[1]] if step[0]=='run' else[str(r/'fixture'),*step[1:]]
 p=subprocess.run(cmd);print('STEP',step,'EXIT',p.returncode,flush=True)
 if p.returncode:break
