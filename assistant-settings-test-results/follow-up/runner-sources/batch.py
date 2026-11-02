import subprocess,sys,pathlib,json
r=pathlib.Path(__file__).resolve().parent
steps=[
 ['fixture','notes','openai','low'],['run','gpt4o-W04'],['fixture','google-break'],['run','gpt4o-W08'],
 ['fixture','reset'],['run','W02-W03'],['fixture','notes','openai','low'],['run','W04'],
 ['fixture','notes','openai','medium','audio'],['run','W01'],['fixture','notes','openai','low'],['run','W05'],
 ['fixture','notes','openai','medium','audio'],['run','W06'],
 ['fixture','notes','openai','low'],['fixture','gem-reset'],['run','W07-W18'],
 ['fixture','google-break'],['run','W08'],['run','W09'],['fixture','google-fix'],
 ['run','upload'],['run','W10'],['run','W11'],['run','W12'],
 ['fixture','reader'],['run','W13'],['fixture','reader-off'],
 ['run','W15'],['fixture','notes','openai','low'],['run','W16'],['run','W17'],['run','F'],
 ['fixture','model','gpt-4o'],['run','gpt4o-F05'],['fixture','model','gpt-6.1-sol'],['run','W14']]
for step in steps:
 cmd=[sys.executable,str(r/'driver.py'),step[1]] if step[0]=='run' else [str(r/'fixture'),*step[1:]]
 p=subprocess.run(cmd);print('STEP',step,'EXIT',p.returncode,flush=True)
 if p.returncode:break
