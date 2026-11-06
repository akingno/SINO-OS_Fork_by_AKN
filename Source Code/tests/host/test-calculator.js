const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {VM,sleep}=require('../../tools/vm-test');
const {calculator,expectDisplay,display,waitFor,scene}=require('../support/calculator-test');
const legacy=process.argv.includes('--legacy');
(async()=>{const vm=new VM(legacy?'calculator-baseline':'calculator-qa'),checks=[];
 const pass=s=>{checks.push(s);console.log('PASS '+s);};
 try{
  await vm.start({fresh:true});await vm.key('f4');let w=await calculator(vm,undefined,legacy);
  const cases=[['12.5+7.25=','19.75'],['99999/3=','33333.00'],['99999*1=','99999.00'],['6*7=','42.00'],['1/0=','Error'],['1/3=','0.33'],['99999.99+0.01=','Error'],['0.5n','-0.50'],['9-12=','-3.00'],['2+3*4=','20.00'],['1.239','1.23'],['99999.99','99999.99']];
  for(const [input,result] of cases){await vm.type('c'+input);await expectDisplay(vm,w,result,legacy);}
  await vm.type('c12.34');await vm.key('backspace');await expectDisplay(vm,w,'12.30',legacy);await vm.key('esc');await expectDisplay(vm,w,'0.00',legacy);
  pass('Keyboard: decimals, limits, signs, truncation, sequential operations, errors, backspace and Esc');
  // All 20 buttons must map to the same input operations. Both = buttons work.
  const expected=['0.00','0.00','0.00','0.00','7.00','8.00','9.00','0.00','4.00','5.00','6.00','0.00','1.00','2.00','3.00','0.00','0.00','0.00','0.00','0.00'];
  for(let i=0;i<20;i++){await vm.type('c');await vm.click(w.x+16+(i%4)*70+31,w.y+124+Math.floor(i/4)*49+20);await expectDisplay(vm,w,expected[i],legacy);}
  await vm.type('c6*7=');await vm.shot('calculator');pass('All twenty mouse buttons and actual framebuffer');
  await vm.key('f4');const second=await calculator(vm,undefined,legacy);assert.notEqual(second.id,w.id);await vm.type('8+9=');await expectDisplay(vm,second,'17.00',legacy);await expectDisplay(vm,w,'42.00',legacy);
  await vm.key('ctrl','w');await waitFor(async()=>!(await vm.state()).windows.some(q=>q.id===second.id),'second close');
  w=await calculator(vm,w.id,legacy);await expectDisplay(vm,w,'42.00',legacy);pass('Multiple independent instances and Ctrl+W');
  if(!legacy){
   assert.equal(w.app,9);assert(w.pid>0);assert((await scene(vm,w)).length<=64);
   await vm.click(w.x+16+65,w.y+130);await expectDisplay(vm,w,'42.00');pass('User process ownership, bounded display list, button gaps');
   await vm.key('ctrl','spc');await vm.type('c6*7=');await expectDisplay(vm,w,'42.00');await vm.key('ctrl','spc');pass('Global Pinyin mode does not intercept calculator keys');
  }
  await vm.key('ctrl','w');await sleep(150);assert(!(await vm.state()).windows.some(q=>q.app===(legacy?3:9)));assert(!vm.log().includes('KERNEL PANIC'));pass('Close and healthy desktop');
  if(!legacy){
   const shell=async command=>{await vm.key('f2');await vm.type(command+'\n');};
   const close=async()=>{await vm.key('ctrl','w');await sleep(100);};
   const open=async file=>{await vm.key('ctrl','o');await vm.type(file);await vm.key('ret');return calculator(vm);};
   await vm.click(22,13);await vm.click(46,132);await calculator(vm);await close();
   await open('/Applications/Calculator.app');await close();
   pass('SINO menu and original Calculator.app dispatch to the native process');
   for(const command of ['run /boot/calc.snx /selftest','run /boot/./CALC.SNX /selftest','run /boot/calc.snx /spawn-test']){
    const before=(vm.log().match(/CALCULATOR SELFTEST PASS/g)||[]).length;
    await shell(command);await waitFor(async()=>(vm.log().match(/CALCULATOR SELFTEST PASS/g)||[]).length===before+1,command);
    assert(!vm.log().includes('CALCULATOR SELFTEST FAIL'),vm.log());
   }
   await shell('write /home/user/Desktop/Calc.lnk /boot/calc.snx');await close();
   for(const file of ['/boot/calc.snx','/home/user/Desktop/Calc.lnk']){w=await open(file);await expectDisplay(vm,w,'0.00');await close();}
   pass('Actual Ring 3 arithmetic selftest, canonical/case aliases, child spawn and shortcut launches');
   await shell('run /boot/calc.snx /title-test');
   await waitFor(async()=>vm.log().includes('CALC TITLE TEST HANDLE '),'title test owner');
   const handle=vm.log().match(/CALC TITLE TEST HANDLE (\d+)/)[1];
   await shell('run /boot/probe.snx /foreign-title:'+handle);
   await waitFor(async()=>vm.log().includes('FOREIGN WINDOW TITLE DENIED PASS'),'foreign title refused');
   await close();w=await calculator(vm);assert.equal(w.title,'计算器');await close();pass('A second process cannot rename a live foreign calculator window');
   await sleep(150);const beforePages=await vm.value('free_pages');
   const refs=async()=>{const b=await vm.memory(vm.symbols.objects,64*16);let sum=0;for(let i=0;i<64;i++)sum+=b.readUInt32LE(i*16+4);return sum;};
   const beforeRefs=await refs();
   for(let i=0;i<4;i++){
    await vm.key('f4');w=await calculator(vm);await vm.type('6*7=');await expectDisplay(vm,w,'42.00');
    if(i===0){await vm.drag(w.x+100,w.y+15,450,120);const moved=await calculator(vm,w.id);assert(moved.x!==w.x||moved.y!==w.y);w=moved;}
    await close();await waitFor(async()=>await vm.value('free_pages')===beforePages,'calculator page reclamation');assert.equal(await refs(),beforeRefs);
   }
   pass('Repeated close reclaims address spaces, kernel stacks, handles, objects and scenes');
   // Kill one calculator using the real F10 task manager, then close the manager.
   await vm.key('f4');w=await calculator(vm);await vm.key('f10');
   const tasks=await waitFor(async()=>(await vm.state()).windows.find(q=>q.title.includes('任务管理器')),'F10 task manager');
   await vm.click(tasks.x+100,tasks.y+104);await vm.click(tasks.x+80,tasks.y+388);
   await waitFor(async()=>!(await vm.state()).windows.some(q=>q.id===w.id),'F10 terminates calculator');await close();
   await waitFor(async()=>await vm.value('free_pages')===beforePages,'F10 page reclamation');pass('Trusted Ring 3 F10 manager terminates calculator without a stale window');
   for(let i=0;i<8;i++){await vm.key('f4');await calculator(vm);}
   assert.equal((await vm.state()).windows.length,8);await vm.key('f4');assert.equal((await vm.state()).modal,2);await vm.key('ret');
   for(let i=0;i<8;i++)await close();
   await waitFor(async()=>await vm.value('free_pages')===beforePages,'full window table reclaimed');assert.equal(await refs(),beforeRefs);
   pass('Window exhaustion reports visibly and all eight instances are reclaimable');
   const logStart=vm.log().length;
   const launched=()=>[...vm.log().slice(logStart).matchAll(/PROCESS start spin.snx pid=0x([0-9A-F]+)/g)].map(m=>parseInt(m[1],16));
   for(let i=0;i<6;i++){
    await shell('run /boot/spin.snx');
    await waitFor(()=>launched().length===i+1,'spin fills process slot '+(i+1),20000);
   }
   await vm.key('f4');assert.equal((await vm.state()).modal,2);await vm.key('ret');
   const spins=launched();assert.equal(spins.length,6);
   const {terminalText}=require('./test-terminal-replacement');
   const term=(await vm.state()).windows.find(w=>w.title==='SINO Shell');
   for(const pid of spins){
    await shell('kill '+pid);
    await waitFor(async()=>{
     const screen=await terminalText(vm,term.id,false);
     return screen.includes('$ kill '+pid+'\n')&&screen.endsWith('gsh:/ $');
    },'owned spin stops: '+pid,20000);
   }
   await close();
   await waitFor(async()=>await vm.value('free_pages')===beforePages,'full process table reclaimed');pass('Process exhaustion, preempted loops, failure notice and recovery');
   await vm.key('f4');w=await calculator(vm);await vm.type('6*7=');
   await vm.key('f7');
   let settings=(await vm.state()).windows.find(q=>q.app===7);
   const click=async(x,y)=>{const width=await vm.value('gui_width'),height=await vm.value('gui_height');await vm.q.call('input-send-event',{events:[{type:'abs',data:{axis:'x',value:Math.round(x*32767/(width-1))}},{type:'abs',data:{axis:'y',value:Math.round(y*32767/(height-1))}}]});await vm.q.call('input-send-event',{events:[{type:'btn',data:{button:'left',down:true}}]});await vm.q.call('input-send-event',{events:[{type:'btn',data:{button:'left',down:false}}]});await sleep(120);};
   const at=async(x,y)=>click(settings.x+x,settings.y+y);
   const apply=async()=>{await at(settings.w-86,settings.h-34);assert.equal(await vm.value('settings_pending'),1);settings=(await vm.state()).windows.find(q=>q.app===7);await at(settings.w-86,settings.h-49);await waitFor(async()=>await vm.value('settings_pending')===0,'settings confirmed');};
   await at(50,196);await at(515,160);await apply();
   await waitFor(async()=>(await vm.state()).windows.find(q=>q.id===w.id)?.title==='Calculator','English title');await expectDisplay(vm,w,'42.00');
   assert((await scene(vm,w)).some(c=>c.text==='Two decimal places'));
   await at(280,160);await apply();await waitFor(async()=>(await vm.state()).windows.find(q=>q.id===w.id)?.title==='计算器','Chinese title');
   await at(50,152);await at(258,141);await at(230,257);await apply();assert.equal(await vm.value('gui_width'),800);await close();
   w=await calculator(vm,w.id);await expectDisplay(vm,w,'42.00');await vm.shot('calculator-800');await close();
   pass('Live bilingual title/content updates preserve arithmetic; 800x600 display');
  }
  fs.writeFileSync(path.join(vm.run,'report.json'),JSON.stringify({result:'PASS',legacy,checks,log:vm.log()},null,2));
 }catch(e){try{await vm.shot('failure');}catch{}fs.writeFileSync(path.join(vm.run,'report.json'),JSON.stringify({result:'FAIL',legacy,checks,error:e.stack,log:vm.log()},null,2));throw e;}finally{await vm.stop();}
})().catch(e=>{console.error(e);process.exitCode=1;});
