const base62='0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
function noteReference(id) {
  let encoded = '';
  while (id > 0) {
    encoded = base62[id % 62] + encoded;
    id = Math.floor(id / 62);
  }
  return `##${encoded.padStart(6, '0')}`;
}

const stable=v=>JSON.stringify(v,(_,x)=>x&&typeof x==='object'&&!Array.isArray(x)?Object.fromEntries(Object.entries(x).sort()):x);
const wait = ms => new Promise(r=>setTimeout(r,ms));
let step='startup', assertions=[], trace=[], events=[];
function check(v,name,actual){ assertions.push({name,ok:!!v,...(v?{}:{actual})});if(!v)throw Error(name+': '+JSON.stringify(actual)); }
async function until(name, fn, timeout=20000){let end=Date.now()+timeout;while(Date.now()<end){let v=await fn();if(v)return v;await wait(30);}throw Error('Timeout: '+name);}
const button=(name,root=document)=>[...root.querySelectorAll('button,[role="tab"]')].find(n=>n.getClientRects().length&&(n.getAttribute('aria-label')??n.querySelector('.cha-primary-line,.cha-roster-name')?.textContent??n.textContent).trim()===name);
const click=async(name,root=document)=>(await until(name,()=>{let b=button(name,root);return b&&!b.disabled&&b})).click();
const row=name=>[...document.querySelectorAll('[role="treeitem"]')].find(n=>n.getAttribute('aria-label')===name);
const editor=()=>document.querySelector('.ProseMirror')?.chaEditor;
async function value(n,t){Object.getOwnPropertyDescriptor(n.tagName==='TEXTAREA'?HTMLTextAreaElement.prototype:HTMLInputElement.prototype,'value').set.call(n,t);n.dispatchEvent(new Event('input',{bubbles:true}));await wait(50);}
async function screenshot(name){await window.webkit.messageHandlers.chaTestSnapshot.postMessage(name);}
let rpc, epoch=0;
async function bridge(){
 const connection=window.__CHA_NATIVE_CONNECTION_ID__,pending=new Map();let next=9000000000000;
 const post=v=>window.__CHA_NATIVE_POST__(JSON.stringify(v));
 window.__CHA_NATIVE_RECEIVE__=batch=>{try{if(batch.connection_id!==connection)return;for(let r of batch.messages){if(r.event){if(r.event==='app.contextChanged')events.push(r);if(r.event==='app.contextChanged')epoch=Math.max(epoch,r.context_epoch);continue;}let done=pending.get(r.id);if(done){pending.delete(r.id);done(r);}}}finally{post({connection_id:batch.connection_id,delivery_id:batch.delivery_id});}};
 rpc=(method,params={})=>new Promise((resolve,reject)=>{let id=next++,timer=setTimeout(()=>{pending.delete(id);reject(Error(method+' timed out'));},25000);pending.set(id,r=>{clearTimeout(timer);if(method!=='session.snapshot'||!r.result?.generation?.active)trace.push({method,params,result:r});if(r.ok)resolve(r.result);else reject(Error(method+': '+JSON.stringify(r.error)));});post({connection_id:connection,id,context_epoch:epoch,method,params});});
 let b=await rpc('app.bootstrap');epoch=b.context_epoch;return b;
}
function doc(text){return {type:'doc',content:text?[{type:'paragraph',content:[{type:'text',text}]}]:[{type:'paragraph'}]};}
async function note(symbol,text,name=symbol,parent_id=null){let n=C.notes[symbol]??await rpc('notes.create',{parent_id,kind:'note',name});await rpc('notes.save',{id:n.id,document:doc(text)});n.ref_id=noteReference(n.id);C.notes[symbol]={...n,text,selected:text,document:doc(text)};let saved=await rpc('notes.get',{id:n.id});check(stable(saved.document)===stable(doc(text)),symbol+' original saved',saved);return C.notes[symbol];}
async function reference(symbol,type='forum',owner='lobby'){return rpc('fileReference.create',{owner_type:type,owner_id:owner,kind:'note',ref_id:C.notes[symbol].ref_id});}
async function unref(symbol,type='forum',owner='lobby'){return rpc('fileReference.delete',{owner_type:type,owner_id:owner,kind:'note',ref_id:C.notes[symbol].ref_id});}
async function session(label,forum='lobby'){let n=await rpc('session.create',{forum_id:forum,label});let id={forum_id:forum,session_id:n.id};await rpc('session.open',id);return id;}
async function submit(id,text,finish=true){await rpc('session.open',id);let r=await rpc('session.submit',{...id,input:{text}});check(r.clear_input!==false,'prompt accepted '+text,r);if(finish)await idle(id);return r;}
async function idle(id){return until('generation idle',async()=>{let s=await rpc('session.snapshot',id);return !s.generation.active&&s;},180000);}
async function compress(symbol,expected){let n=C.notes[symbol];await rpc('notes.compressStart',{id:n.id});await until('compression done '+symbol,async()=>!(await rpc('notes.compressionStatus',{id:n.id})).pending,45000);let v=await rpc('notes.get',{id:n.id});check(v.compressed_text===expected,symbol+' compressed exact',{expected,actual:v.compressed_text});check(stable(v.document)===stable(n.document),symbol+' original unchanged',v.document);n.selected=expected;return v;}
async function edit(symbol,text,name){let n=C.notes[symbol];if(name){let a=await rpc('notes.rename',{id:n.id,name});n.name=a.name;}n.text=text;n.document=doc(text);await rpc('notes.save',{id:n.id,document:n.document});}
async function add(id,symbol){return submit(id,C.notes[symbol].ref_id);}
async function nativeParity(){try{await until('composer ready',()=>document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));await phase();return {ok:true,C,assertions,trace,events};}catch(e){return {ok:false,step,reason:String(e),C,assertions,trace,events,screen:document.body.innerText.slice(-4000)};}}


async function checkpoint(name){await screenshot(name);await until('export '+name,async()=>{try{return (await rpc('notes.readTextFile',{path:'/private/tmp/cha-ass-instructions/checkpoints/'+name+'.txt'}))?.text==='ready'}catch(e){return false}},30000);}
async function ask(key,text){await submit(C.welcome,text);C[key]=await rpc('session.snapshot',C.welcome);C[key+'Settings']=await rpc('notesSettings.get');}
