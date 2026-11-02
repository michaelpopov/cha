import subprocess,sys,json,pathlib
r=pathlib.Path(__file__).resolve().parent
steps=[['run','W05'],['fixture','notes','openai','medium','audio'],['run','W01'],['run','W06'],['fixture','notes','openai','low'],['run','W07-W18'],['fixture','google-break'],['run','W08'],['run','W09'],['run','upload'],['run','W10'],['run','W11'],['run','W12'],['run','W14'],['run','W15'],['run','W16'],['run','W17'],['run','F']]
for step in steps:
 cmd=[sys.executable,str(r/'run.py'),step[1]] if step[0]=='run' else [str(r/'fixture'),*step[1:]]
 p=subprocess.run(cmd)
 print('STEP',step,'EXIT',p.returncode,flush=True)
 if step[0]=='fixture' and p.returncode:break
