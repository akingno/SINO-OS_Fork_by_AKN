const assert=require('node:assert/strict');
async function run(api){const {check,launch,processes,resources,until,type,key,text,waitText}=api;const base=await resources();let pid;
const prompt=()=>until(async()=>(await text(pid)).endsWith('gsh:/ $'),'editor prompt');
await check('Shell raw edit cursor, delete, history, completion and clear',async()=>{
 await key('f12');await until(async()=>(await processes()).some(p=>p.name==='term.snx'),'formal F12 Shell');pid=(await processes()).find(p=>p.name==='term.snx').pid;await prompt();await type('echo ac');await key('left');await type('b');await key('end');await key('ret');await waitText(pid,'abc');await prompt();
 await key('up');await key('home');await key('end');await key('ret');await prompt();
 await type('hel');await key('tab');await key('ret');await waitText(pid,'builtins:');await prompt();
 await type('echo axbd');await key('home');for(let i=0;i<6;i++)await key('right');await key('delete');await key('end');await key('ret');await waitText(pid,'abd');await prompt();
 await type('cat wel');await key('tab');await key('ret');await waitText(pid,'Welcome to SINO Platinum Desktop.');await prompt();
 await type('echo ');await key('ctrl','spc');await type('nihao');await key('spc');await key('ctrl','spc');await key('backspace');await type('X');await key('ret');await waitText(pid,'你X');await prompt();
 await launch('rdtest','clip');await until(async()=>(await processes()).every(p=>p.name!=='rdtest.snx'),'clipboard fixture');
 await key('ctrl','v');await until(async()=>(await text(pid)).endsWith('gsh:/ $ echo pasted'),'paste completes without execution');await key('ret');await waitText(pid,'pasted');await prompt();
 await launch('rdtest','clip-bad');await until(async()=>(await processes()).every(p=>p.name!=='rdtest.snx'),'bad clipboard fixture');await key('ctrl','v');await prompt();assert(!(await text(pid)).includes('gsh:/ $ echo one'));
 await type('clear');await key('ret');await prompt();assert(!(await text(pid)).includes('Welcome to SINO Platinum Desktop.'));
 await type('echo keep');await key('ctrl','l');await key('ret');await waitText(pid,'keep');await prompt();
 await type('broken');await key('ctrl','c');await type('echo alive\n');await waitText(pid,'alive');await prompt();
 await key('ctrl','w');await until(async()=>(await processes()).length===0,'edit cleanup');assert.deepEqual(await resources(),base);
});}
module.exports={run};
