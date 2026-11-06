/* Inspect the rendered calculator scene, never its private arithmetic state. */
const assert = require('node:assert/strict');
const {sleep} = require('../../tools/vm-test');
async function waitFor(read, label, timeout=5000) {
 const end=Date.now()+timeout;
 do {const value=await read();if(value)return value;await sleep(25);} while(Date.now()<end);
 throw Error('Timed out: '+label);
}
async function owner(vm,w){return (await vm.memory(vm.symbols.user_owner+w.id*4,4)).readUInt32LE();}
async function scene(vm,w){
 const address=(await vm.memory(vm.symbols.user_scene+w.id*4,4)).readUInt32LE();
 if(!address)return [];
 const count=(await vm.memory(address,4)).readUInt32LE();assert(count<=256);
 const data=await vm.memory(address+4,count*104),commands=[];
 for(let i=0;i<count;i++){const b=data.subarray(i*104,(i+1)*104);commands.push({kind:b.readInt32LE(),color:b.readUInt32LE(20),x:b.readInt32LE(4),y:b.readInt32LE(8),w:b.readInt32LE(12),h:b.readInt32LE(16),text:b.subarray(24,104).toString().split('\0')[0]});}
 return commands;
}
async function calculator(vm,id,legacy=false){
 return waitFor(async()=>{
  const s=await vm.state();
  for(const w of s.windows){
   if(id!==undefined?w.id!==id:w.id!==s.focus)continue;
   if(legacy&&w.app===3)return w;
   if(w.app!==9)continue;
   const pid=await owner(vm,w);
   if(!vm.log().includes('PROCESS start calc.snx pid=0x'+pid.toString(16).toUpperCase().padStart(8,'0')))continue;
   if((await scene(vm,w)).some(c=>c.kind===2&&c.y===65))return {...w,pid};
  }
 },'calculator window');
}
async function display(vm,w,legacy=false){
 if(legacy){const current=(await vm.state()).windows.find(q=>q.id===w.id);return current.error?'Error':(current.value/100).toFixed(2);}
 const c=(await scene(vm,w)).find(c=>c.kind===2&&c.y===65);
 return c?.text==='错误'?'Error':c?.text;
}
async function expectDisplay(vm,w,expected,legacy=false){
 await waitFor(async()=>await display(vm,w,legacy)===expected,'display '+expected);
 assert.equal(await display(vm,w,legacy),expected);
}
module.exports={waitFor,calculator,display,expectDisplay,scene,owner};
