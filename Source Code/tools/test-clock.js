const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {VM,sleep}=require('./vm-test');const {clock,clockTime,topTime,waitFor,scene}=require('./clock-test');
const legacy=process.argv.includes('--legacy');
(async()=>{const vm=new VM(legacy?'clock-baseline':'clock-qa'),checks=[];const pass=s=>{checks.push(s);console.log('PASS '+s);};
 try{
  await vm.start({fresh:true});await vm.key('f6');let w=await clock(vm,undefined,legacy);
  const first=await waitFor(async()=>{const text=await clockTime(vm,w,legacy);return /^\d{2}:\d{2}:\d{2}$/.test(text)?text:false;},'first valid RTC sample',5000);
  await waitFor(async()=>await clockTime(vm,w,legacy)!==first,'clock actually updates',5000);await vm.shot('clock');pass('F6 starts a live analog/digital clock');
  for(let i=0;i<8;i++){await vm.key('f6');assert.equal((await clock(vm,undefined,legacy)).id,w.id);}
  assert.equal((await vm.state()).windows.length,1);
  await vm.click(w.x+w.w-39,w.y+15);assert.equal((await vm.state()).windows.find(q=>q.id===w.id).minimized,1);await vm.key('f6');w=await clock(vm,w.id,legacy);assert.equal(w.minimized,0);
  await vm.click(22,13);await vm.click(46,legacy?188:160);assert.equal((await clock(vm,undefined,legacy)).id,w.id);pass('Repeated F6, minimize/restore and SINO menu keep a single clock');
  await vm.key('f1');await vm.key('ctrl','o');await vm.type('/Applications/Clock.app');await vm.key('ret');assert.equal((await clock(vm,undefined,legacy)).id,w.id);await vm.key('ctrl','w');await vm.key('ctrl','w');
  await waitFor(async()=>(await vm.state()).windows.length===0,'clock and file browser close');pass('Original Clock.app dispatch and Ctrl+W');
  const top=await topTime(vm);await waitFor(async()=>await topTime(vm)!==top,'top bar independent of clock',5000);pass('Top bar keeps time after the clock application closes');
  if(!legacy){
   const {execFileSync}=require('node:child_process'),host=require('./host-tools');
   const rtcExe=path.join(vm.run,'rtc-test.exe');
   execFileSync(host.tool('gcc'),[path.join(__dirname,'test-rtc.c'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-o',rtcExe],{windowsHide:true});
   const rtcResult=execFileSync(rtcExe,[],{encoding:'utf8',windowsHide:true});assert(rtcResult.includes('RTC DRIVER PASS'));fs.writeFileSync(path.join(vm.run,'rtc-test.txt'),rtcResult);pass(rtcResult.trim());
   const close=async()=>{await vm.key('ctrl','w');await sleep(120);};
   const shell=async command=>{await vm.key('f2');await vm.type(command+'\n');};
   for(const command of ['run /boot/clock.snx /selftest','run /boot/./CLOCK.SNX /selftest','run /boot/clock.snx /spawn-test']){
    const n=(vm.log().match(/CLOCK SELFTEST PASS/g)||[]).length;await shell(command);await waitFor(async()=>(vm.log().match(/CLOCK SELFTEST PASS/g)||[]).length===n+1,command);assert(!vm.log().includes('CLOCK SELFTEST FAIL'),vm.log());
   }
   await shell('date');await shell('time');assert(!vm.log().includes('RTC time unavailable'));await close();pass('Caps=0 wall time, canonical/case and child launches; Shell date/time');
   const before=await vm.value('free_pages');
   const refs=async()=>{const b=await vm.memory(vm.symbols.objects,64*16);let sum=0;for(let i=0;i<64;i++)sum+=b.readUInt32LE(i*16+4);return sum;};const beforeRefs=await refs();
   await vm.key('f6');w=await clock(vm);assert.equal(w.app,9);
   // Pause for a coherent display-list/backing-surface sample.
   await vm.q.call('stop');let commands;
   try{
    commands=await scene(vm,w);assert.equal(commands.length,69);assert.equal(commands.filter(c=>c.kind===4).length,63);assert.equal(commands.filter(c=>c.kind===8).length,3);
    const [hour,minute,second]=commands.find(c=>c.kind===2&&c.y===297).text.split(':').map(Number);
    const sine=Array.from({length:60},(_,i)=>Math.round(Math.sin(i*Math.PI/30)*100));
    const expected=[[hour%12*5+Math.floor(minute/12),56],[minute,84],[second,94]];
    for(let i=0;i<3;i++){const line=commands[63+i],[pos,len]=expected[i];assert.equal(line.x,165);assert.equal(line.y,171);assert.equal(line.w,Math.trunc(sine[pos]*len/100)||0);assert.equal(line.h,-Math.trunc(sine[(pos+15)%60]*len/100)||0);}
    const pixels=async(x,y)=>(await vm.memory(0x1c00000+w.id*0x300000+(y*w.w+x)*4,4)).readUInt32LE();
    assert.equal(await pixels(165,62),0x858d99);assert.equal(await pixels(56,62),0xd6d7db);assert.equal(await pixels(165,65),0xf7f7f3);
   }finally{await vm.q.call('cont');}
   pass('69-command user canvas, actual three-hand geometry and ellipse pixels');
   const last=await clockTime(vm,w);await vm.q.call('stop');await sleep(1200);await vm.q.call('cont');await waitFor(async()=>await clockTime(vm,w)!==last,'clock resumes from RTC',5000);
   await vm.drag(w.x+100,w.y+15,450,120);let moved=await clock(vm,w.id);assert(moved.x!==w.x||moved.y!==w.y);await close();
   await waitFor(async()=>await vm.value('free_pages')===before,'clock page reclaim');assert.equal(await refs(),beforeRefs);pass('Pause/resume, dragging and full page/object reclamation');
   for(let i=0;i<3;i++){await vm.key('f6');await clock(vm);await close();await waitFor(async()=>await vm.value('free_pages')===before,'repeat clock reclaim');}
   await vm.key('f6');w=await clock(vm);await vm.key('f10');const tasks=await waitFor(async()=>(await vm.state()).windows.find(q=>q.title.includes('任务管理器')),'F10 tasks');await vm.click(tasks.x+100,tasks.y+104);await vm.click(tasks.x+80,tasks.y+388);await waitFor(async()=>!(await vm.state()).windows.some(q=>q.id===w.id),'clock killed');await close();await waitFor(async()=>await vm.value('free_pages')===before,'killed clock reclaimed');
   await vm.key('f6');w=await clock(vm);await close();await waitFor(async()=>await vm.value('free_pages')===before,'clock restart');pass('Repeated close, F10 termination and singleton PID recovery');
   const faults=await vm.value('process_faults');await shell('run /boot/clock.snx /fault-test');w=await clock(vm);
   await waitFor(async()=>await vm.value('process_faults')===faults+1,'actual clock fault contained');await waitFor(async()=>!(await vm.state()).windows.some(q=>q.id===w.id),'fault window reclaimed');await close();await waitFor(async()=>await vm.value('free_pages')===before,'fault process reclaimed');
   const afterFault=await topTime(vm);await waitFor(async()=>await topTime(vm)!==afterFault,'top bar after clock fault');pass('Fault in the real clock process is isolated; top bar and resources survive');
   for(let i=0;i<8;i++){await vm.key('f4');await waitFor(async()=>(await vm.state()).windows.filter(q=>q.app===9).length===i+1,'fill windows');}await vm.key('f6');assert.equal((await vm.state()).modal,2);await vm.key('ret');for(let i=0;i<8;i++)await close();await waitFor(async()=>await vm.value('free_pages')===before,'full windows reclaim');
   const logStart=vm.log().length;const launched=()=>[...vm.log().slice(logStart).matchAll(/PROCESS start spin.snx pid=0x([0-9A-F]+)/g)].map(m=>parseInt(m[1],16));
   for(let i=0;i<6;i++){
    await shell('run /boot/spin.snx');
    await waitFor(()=>launched().length===i+1,'spin fills process slot '+(i+1),20000);
   }
   await vm.key('f6');assert.equal((await vm.state()).modal,2);await vm.key('ret');const spins=launched();assert.equal(spins.length,6);const {terminalText}=require('./test-terminal-replacement');
   const term=(await vm.state()).windows.find(w=>w.title==='SINO Shell');
   for(const pid of spins){
    await shell('kill '+pid);
    await waitFor(async()=>{
     const screen=await terminalText(vm,term.id,false);
     return screen.includes('$ kill '+pid+'\n')&&screen.endsWith('gsh:/ $');
    },'owned spin stops: '+pid,20000);
   }
   await close();await waitFor(async()=>await vm.value('free_pages')===before,'full processes reclaim');await vm.key('f6');w=await clock(vm);pass('Full window/process tables fail visibly and singleton can recover');
   await vm.key('f7');let settings=(await vm.state()).windows.find(q=>q.app===7);
   const click=async(x,y)=>{const width=await vm.value('gui_width'),height=await vm.value('gui_height');await vm.q.call('input-send-event',{events:[{type:'abs',data:{axis:'x',value:Math.round(x*32767/(width-1))}},{type:'abs',data:{axis:'y',value:Math.round(y*32767/(height-1))}}]});await vm.q.call('input-send-event',{events:[{type:'btn',data:{button:'left',down:true}}]});await vm.q.call('input-send-event',{events:[{type:'btn',data:{button:'left',down:false}}]});await sleep(120);};
   const at=async(x,y)=>click(settings.x+x,settings.y+y);const apply=async()=>{await at(settings.w-86,settings.h-34);assert.equal(await vm.value('settings_pending'),1);settings=(await vm.state()).windows.find(q=>q.app===7);await at(settings.w-86,settings.h-49);await waitFor(async()=>await vm.value('settings_pending')===0,'settings confirmed');};
   await at(50,196);await at(515,160);await apply();await waitFor(async()=>(await vm.state()).windows.find(q=>q.id===w.id)?.title==='Clock','English clock');assert((await scene(vm,w)).some(c=>c.text==='Hardware clock'));
   await at(280,160);await apply();await waitFor(async()=>(await vm.state()).windows.find(q=>q.id===w.id)?.title==='时钟','Chinese clock');
   await at(50,152);await at(258,141);await at(230,257);await apply();assert.equal(await vm.value('gui_width'),800);await close();w=await clock(vm,w.id);assert(/^\d{2}:\d{2}:\d{2}$/.test(await clockTime(vm,w)));await vm.shot('clock-800');await close();pass('Bilingual clock and 800x600 display');
  }
  assert(!vm.log().includes('KERNEL PANIC'));
  fs.writeFileSync(path.join(vm.run,'report.json'),JSON.stringify({result:'PASS',legacy,checks,log:vm.log()},null,2));
 }catch(e){try{await vm.shot('failure');}catch{}fs.writeFileSync(path.join(vm.run,'report.json'),JSON.stringify({result:'FAIL',legacy,checks,error:e.stack,log:vm.log()},null,2));throw e;}finally{await vm.stop();}
})().catch(e=>{console.error(e);process.exitCode=1;});
