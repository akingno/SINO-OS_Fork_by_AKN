/* Read public native scenes/ownership; legacy mode is only for the baseline. */
const assert=require('node:assert/strict');
const {waitFor,scene,owner}=require('./ui-test');
async function files(vm,id,legacy=false){return waitFor(async()=>{
 const s=await vm.state();for(const w of s.windows){
  if(id===undefined?w.id!==s.focus:w.id!==id)continue;
  if(legacy&&w.app===1)return w;
  if(w.app!==9)continue;const pid=await owner(vm,w);
  if(vm.log().includes('PROCESS start files.snx pid=0x'+pid.toString(16).toUpperCase().padStart(8,'0'))&&(await scene(vm,w)).some(c=>c.text.startsWith('/'))){const commands=await scene(vm,w),folderPath=commands.find(c=>c.x===12&&c.y===w.h-25)?.text;const all=await require('./paint-fixture').nodes(vm),parent=all.find(n=>n.path===folderPath);const sel=commands.find(c=>c.kind===1&&c.color===0x355e9a);const name=sel&&commands.find(c=>c.x===181&&c.y===sel.y+3)?.text;return {...w,pid,folderPath,folder:parent?.id,selected:all.find(n=>n.parent===parent?.id&&n.name===name)?.id??-1};}
 }
},'Files user window');}
async function rowPoint(vm,w,name,legacy=false){
 if(!legacy){const c=await waitFor(async()=>(await scene(vm,w)).find(c=>c.text===name&&c.y>=106&&c.y<w.h-28),'visible file row '+name);return [w.x+230,w.y+c.y+8];}
 const b=await vm.memory(vm.symbols.nodes,124*128);let row=0;for(let i=1;i<128;i++){const n=b.subarray(i*124,(i+1)*124);if(n.readInt32LE(32)&&n.readInt32LE(36)===w.folder){if(n.subarray(0,32).toString().split('\0')[0]===name)return [w.x+230,w.y+118+row*28];row++;}}throw Error('Missing row '+name);
}
async function expectInput(vm){return waitFor(async()=>{if((await vm.state()).modal===1)return true;const s=await vm.state(),w=s.windows.find(w=>w.id===s.focus);return w?.app===9&&(await scene(vm,w)).some(c=>c.x===44&&c.y===154);},'input dialog');}
async function expectNotice(vm){return waitFor(async()=>{if((await vm.state()).modal===2)return true;const s=await vm.state(),w=s.windows.find(w=>w.id===s.focus);return w?.app===9&&(await scene(vm,w)).some(c=>c.x===40&&c.y===86);},'notice');}
module.exports={files,rowPoint,expectInput,expectNotice};
