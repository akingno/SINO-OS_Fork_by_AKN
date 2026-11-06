const {waitFor,scene,owner}=require('./calculator-test');
async function clock(vm,id,legacy=false){return waitFor(async()=>{
 const s=await vm.state();for(const w of s.windows){
  if(id!==undefined?w.id!==id:w.id!==s.focus)continue;
  if(legacy&&w.app===5)return w;
  if(w.app!==9)continue;const pid=await owner(vm,w);
  if(!vm.log().includes('PROCESS start clock.snx pid=0x'+pid.toString(16).toUpperCase().padStart(8,'0')))continue;
  if((await scene(vm,w)).some(c=>c.kind===2&&c.y===297))return {...w,pid};
 }
},'clock window');}
async function topTime(vm){return (await vm.memory(vm.symbols.clock_text,9)).toString().split('\0')[0];}
async function clockTime(vm,w,legacy=false){if(legacy)return topTime(vm);return (await scene(vm,w)).find(c=>c.kind===2&&c.y===297)?.text;}
module.exports={clock,clockTime,topTime,waitFor,scene,owner};
