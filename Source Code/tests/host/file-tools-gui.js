/* Real Shell/broker/file objects and raw pager keys; no pixel comparisons. */
const assert=require('node:assert/strict');
async function run(api){
 const {check,launch,processes,resources,until,key,type,text,waitText,command,window,memory,symbols,qmp,value}=api;
 let first,second,baseline,initial;
 async function complete(pid,line){await type(line+'\n');await until(async()=>/gsh:\/[^\n]* \$$/.test(await text(pid)),'G4 prompt');}
 async function row(pid,line){await until(async()=>(await text(pid)).split('\n').includes(line),line);}
 async function more(pid){await until(async()=>(await text(pid)).endsWith('-- more: Space / q --')&&(await processes()).some(p=>p.name==='tview.snx'),'fresh pager pause');}
 async function closeAll(){await key('ctrl','w');await until(async()=>(await processes()).length===0,'G4 cleanup');assert.deepEqual(await resources(),baseline);}
 await check('G4 readonly commands, cwd paths, options and confinement',async()=>{
  initial=await resources();await launch('rdtest','fixtures');await until(async()=>(await processes()).length===0,'G4 fixtures ready');
  baseline=await resources();first=await launch('term','');await waitText(first,'gsh:/ $');
  await complete(first,'ls Documents/g4');await row(first,'tiny.txt');
  assert(!(await text(first)).split('\n').includes('.hidden'));
  await complete(first,'ls -la Documents/g4');await waitText(first,'f 5 tiny.txt');await waitText(first,'f 0 .hidden');
  await type('cd Documents/g4\n');await waitText(first,'gsh:/Documents/g4 $');
  await complete(first,'cat tiny.txt');await row(first,'hello');
  await complete(first,'stat tiny.txt');await row(first,'length=5');await row(first,'kind=file');
  await complete(first,'cat empty.txt');await type('status\n');await row(first,'status code=0 reason=1 start=0');
  await complete(first,'cat missing.txt');await waitText(first,'file error -4');
  await complete(first,'ls -z');await waitText(first,'usage: ls [-a|-l|-la|-al] [path]');
  await complete(first,'cat ../../../boot/readme.txt');await waitText(first,'path outside grant or too long');
  await complete(first,'cat binary.txt');await row(first,'A??B');
  await complete(first,'cat split.txt');await waitText(first,'你Z');
  await complete(first,'cat big.txt');await waitText(first,'G4-END');
  await key('ctrl','w');await until(async()=>(await processes()).length===0,'G4 read tools close');assert.deepEqual(await resources(),baseline);
 });
 await check('G4 pager raw keys, pages, quit, EOF, cancel and two sessions',async()=>{
  first=await launch('term','');await waitText(first,'gsh:/ $');
  await type('view Documents/g4/pages.txt\n');await more(first);
  assert(!(await text(first)).includes('PAGE-END'));
  await key('spc');await waitText(first,'PAGE-END');await until(async()=>/gsh:\/[^\n]* \$$/.test(await text(first)),'pager complete');
  await type('view Documents/g4/pages.txt\n');await more(first);await type('q');await until(async()=>/gsh:\/[^\n]* \$$/.test(await text(first)),'pager quit');
  await type('view Documents/g4/pages.txt\n');await more(first);await key('ctrl','d');await until(async()=>/gsh:\/[^\n]* \$$/.test(await text(first)),'pager EOF');
  await type('view Documents/g4/pages.txt\n');await more(first);
  second=await launch('term','');await waitText(second,'gsh:/ $');await complete(second,'cat Documents/g4/tiny.txt');await row(second,'hello');
  await key('alt','tab');await key('ctrl','c');await waitText(first,'task ended reason=3');
  await complete(first,'cat Documents/g4/tiny.txt');await row(first,'hello');await key('ctrl','w');await key('ctrl','w');
  await until(async()=>(await processes()).length===0,'both G4 windows closed');assert.deepEqual(await resources(),baseline);
 });
 await check('G4 Chinese paths, long-line paging, resize and close release file handles',async()=>{
  first=await launch('term','');await waitText(first,'gsh:/ $');
  await type('cat Documents/g4/');await key('ctrl','spc');await type('nihao');await key('spc');await key('ctrl','spc');await type('.txt\n');
  await row(first,'Chinese path');await until(async()=>/gsh:\/[^\n]* \$$/.test(await text(first)),'Chinese file command returns');
  await type('view Documents/g4/big.txt\n');await more(first);
  const id=await window(first),stride=await value('gui_window_stride');
  const b=await memory(symbols.windows+id*stride,28);
  await qmp.click(b.readInt32LE(8)+b.readInt32LE(16)-16,b.readInt32LE(12)+15);
  await key('spc');await more(first);await closeAll();
 });
 await check('G4 detects file revision changes while paged and recovers',async()=>{
  first=await launch('term','');await waitText(first,'gsh:/ $');
  await type('view Documents/g4/pages.txt\n');await more(first);
  await command('write /home/user/Documents/g4/pages.txt changed');
  await key('spc');await waitText(first,'file changed; reopen');
  await until(async()=>/gsh:\/[^\n]* \$$/.test(await text(first)),'changed file recovers');
  await complete(first,'cat Documents/g4/pages.txt');await row(first,'changed');await closeAll();
  await launch('rdtest','cleanup');await until(async()=>(await processes()).length===0,'G4 fixtures removed');assert.deepEqual(await resources(),initial);
 });
}
module.exports={run};
