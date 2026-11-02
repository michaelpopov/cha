from pathlib import Path
r=Path('/private/tmp/cha-ass-settings-run')
base="""await bridge();C.welcome={forum_id:'builtin-entrance',session_id:'builtin-welcome'};await rpc('session.open',C.welcome);await submit(C.welcome,'/clear');
async function ask(key,text){await submit(C.welcome,text);C[key]=await rpc('session.snapshot',C.welcome);C[key+'Settings']=await rpc('notesSettings.get');}
"""
cases={
'W04':"""await ask('step1','Use high effort for compression.');await ask('step2','Undo that.');await ask('step3','Undo again.');""",
'W05':"""await ask('answer','Set the note audio service to https://tts.example.com with the Audio key. Go ahead.');""",
'W06':"""await ask('step1','Remove the audio key from Notes settings.');await ask('step2','Put it back.');""",
'W01':"""await ask('answer','What do my Notes settings do?');""",
'W07-W18':"""await ask('W07step1',\"Set Gem's reasoning effort to high.\");await ask('W07step2','Use Gemini for note compression with extra high effort.');await ask('W18','About Gem: I understand that effort has no effect. Save high effort for Gem anyway.');C.gem=await rpc('character.get',{character_id:C.Gem});""",
'W08':"""let s=await session('W08 reproduction',C.lab);await submit(s,'@Gem Say hello.');C.reproduction=await rpc('session.snapshot',s);await ask('step1','Gem fails in Lab. What is wrong?');await ask('step2','Fix it.');let verify=await session('W08 verification',C.lab);await submit(verify,'@Gem Say hello.');C.verification=await rpc('session.snapshot',verify);C.googleProvider=await rpc('provider.get',{provider_id:C.google});""",
'W09':"""await ask('answer','Why does high effort make no difference for Claude Test?');""",
'W11':"""await ask('answer','Explain how Writer can search Docs OpenAI and Docs Gemini.');""",
'W12':"""await ask('answer','Does Lab use my Huge note and Long note? What happens if I edit Long note later?');""",
'W14':"""await ask('step1',\"Add Long note to Writer's Files. Go ahead.\");await ask('step2',\"Remove the Docs Removed folder from Lab's Files. Do it.\");C.forum=await rpc('forum.get',{forum_id:C.lab});""",
'W15':"""let s=await session('S1',C.lab);await rpc('session.setCharacterSettings',{...s,character_id:C.Writer,provider:C.google,reasoning_effort:null});C.sessionSettings=await rpc('session.snapshot',s);await ask('answer','Writer answers in S1 with Gemini, but its settings say OpenAI. Why?');""",
'W16':"""let s=await session('W16 Lab save attempt',C.lab);await submit(s,'Save this conversation for later review.');C.labSave=await rpc('session.snapshot',s);await ask('step1','Why was my Lab conversation not saved?');await ask('step2','Save this conversation for later review.');""",
'W17':"""await ask('step1','Use high effort for yourself and add Long note to your Files.');await ask('step2','Create an API key for my audio service.');""",
'F':"""C.forumSession=await session('Forum Assistant capability tests',C.lab);C.answers=[];for(const text of ['Where do I choose the model that compresses notes?',\"Change Writer's provider to Gemini.\",'What does Default mean in the chat toolbar?','How do I give Writer a folder of documents?','Which API formats can a provider use?','Where are the shared instructions for all characters?']){await submit(C.forumSession,'/clear');await submit(C.forumSession,'@Assistant '+text);C.answers.push(await rpc('session.snapshot',C.forumSession));}""",
'upload':"""C.stores={};for(const [name,service,key]of [['Docs OpenAI','openai','api_key_1'],['Docs Gemini','google','api_key_2'],['Docs Removed','openai','api_key_1']]){let start=await rpc('notes.store.upload',{folder_id:C[name].id,service,api_key_id:key});let result=await until('folder upload '+name,async()=>{let s=await rpc('notes.store.status',{operation_id:start.operation_id});return s.terminal&&s;},240000);C.stores[name]=result;if(result.error)continue;await rpc('fileReference.create',{owner_type:name==='Docs Gemini'?'character':'forum',owner_id:name==='Docs Gemini'?C.Writer:C.lab,kind:'folder',ref_id:C[name].ref_id});if(name==='Docs Removed'){let stores=await rpc('notes.store.list');C.storesBeforeRemove=stores;let row=stores.find?stores.find(x=>x.folder_id===C[name].id):stores.items.find(x=>x.folder_id===C[name].id);let removal=await rpc('notes.store.remove',{folder_id:C[name].id,store_id:row.id});C.removed=await until('folder removed',async()=>{let s=await rpc('notes.store.status',{operation_id:removal.operation_id});return s.terminal&&s;},240000);}}""",
'W10':"""let s=await session('W10 reproduction',C.lab);await submit(s,'@Writer Say hello.');C.reproduction=await rpc('session.snapshot',s);await ask('answer',\"Lab's Docs Removed folder seems to do nothing. Why?\");"""
}
for name,code in cases.items():(r/(name+'.js')).write_text(base+code)
