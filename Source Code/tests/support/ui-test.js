/* Generic GUI test helpers: inspect rendered scenes and windows. */
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
/* The SINO Shell is a kept ring-3 application; tests use it to prove a native
   window launches and renders. Press F2 (or open Terminal.app) first. */
async function shellWindow(vm,id){return waitFor(async()=>{
 const s=await vm.state();
 for(const w of s.windows){
  if(id!==undefined?w.id!==id:w.id!==s.focus)continue;
  if(w.app===9&&w.title==='SINO Shell')return w;
 }
},'shell window');}
module.exports={waitFor,scene,owner,shellWindow};
