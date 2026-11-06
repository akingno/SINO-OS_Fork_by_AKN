const {seedFilesDisk}=require('./files-fixture');
const {waitFor,scene,calculator,expectDisplay}=require('./calculator-test');
const {expectNotice}=require('./files-test');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {VM,sleep}=require('./vm-test'),{files,rowPoint}=require('./files-test'),{nodes,fileBytes:readFileBytes}=require('./paint-fixture');
const fileBytes=async(vm,p)=>waitFor(async()=>{try{return await readFileBytes(vm,p)}catch{return false}},'file available '+p);
const legacy=process.argv.includes('--legacy');
(async()=>{const vm=new VM(legacy?'files-baseline':'files-qa'),checks=[];const pass=s=>{checks.push(s);console.log('PASS '+s);};
 const open=async p=>{await vm.key('ctrl','o');await vm.type(p);await vm.key('ret');};
 const menu=async(row,m=1)=>{const x=m===1?181:22;await vm.click(x,13);await vm.click(x+35,48+row*28);};
 const exists=async p=>(await nodes(vm)).find(n=>n.path===p);
 const select=async name=>{const w=await files(vm,undefined,legacy);await vm.click(...await rowPoint(vm,w,name,legacy));};
 try{
  const large=legacy?null:seedFilesDisk(path.join(vm.run,'data.img'));await vm.start({fresh:legacy});
  for(const route of ['f1','menu','/Applications/Files.app','/home/user/Documents']){
   if(route==='f1')await vm.key('f1');else if(route==='menu')await menu(1,0);else await open(route);
   await files(vm,undefined,legacy);await vm.key('ctrl','w');pass('Launch route '+route);
  }
  await open('/home/user/Documents');await files(vm,undefined,legacy);
  if(!legacy){
   await vm.click(413,13);await vm.click(448,48);await waitFor(async()=>(await vm.state()).modal===2,'global save confirmation');assert.match((await vm.memory(vm.symbols.message,128)).toString(),/个人文件已保存|Home files have been saved/);await vm.key('ret');pass('Global Save Home still invokes shared persistence');
   for(const row of [0,1]){const before=(await vm.state()).windows.length;await vm.click(277,13);await vm.click(312,48+row*28);const child=await files(vm);assert.equal((await vm.state()).windows.length,before+1);assert.equal(child.folderPath,row===0?'/home/user':'/Applications');await vm.key('ctrl','w');}pass('View menu opens independent Home and Applications windows');
  }

  await menu(1);await vm.type('Folder');await vm.key('ret');await waitFor(()=>exists('/home/user/Documents/Folder'),'file committed');
  await menu(2);await vm.type('Alpha.txt');await vm.key('ret');await vm.type('original-content');await vm.key('ctrl','s');await vm.key('ctrl','w');assert.equal((await fileBytes(vm,'/home/user/Documents/Alpha.txt')).toString(),'original-content');pass('New directory and document open/save');
  await select('Alpha.txt');await vm.key('ctrl','c');await vm.key('ctrl','v');assert.equal((await fileBytes(vm,'/home/user/Documents/Copy of Alpha.txt')).toString(),'original-content');
  await select('Copy of Alpha.txt');await menu(6);await vm.type('Renamed.txt');await vm.key('ret');await waitFor(()=>exists('/home/user/Documents/Renamed.txt'),'file committed');pass('Copy without overwrite, rename');
  await select('Renamed.txt');await vm.key('ctrl','x');let w=await files(vm,undefined,legacy);await vm.double(...await rowPoint(vm,w,'Folder',legacy));await vm.key('ctrl','v');await waitFor(()=>exists('/home/user/Documents/Folder/Renamed.txt'),'file committed');assert(!await exists('/home/user/Documents/Renamed.txt'));pass('Navigate in place, cross-folder cut/paste');
  await select('Renamed.txt');await vm.key('delete');await waitFor(()=>exists('/home/user/Trash/Renamed.txt'),'file committed');await vm.key('ctrl','w');await open('/home/user/Trash');await files(vm,undefined,legacy);await select('Renamed.txt');await menu(7);await vm.type('/home/user/Documents/Restored.txt');await vm.key('ret');await waitFor(()=>exists('/home/user/Documents/Restored.txt'),'file committed');pass('Trash and restore to an explicit path');
  await vm.key('ctrl','w');await open('/home/user/Documents');await files(vm,undefined,legacy);await select('Restored.txt');await vm.key('delete');await vm.key('ctrl','w');await open('/home/user/Trash');await files(vm,undefined,legacy);await select('Restored.txt');await vm.key('delete');assert(!await exists('/home/user/Trash/Restored.txt'));pass('Delete within Trash is permanent');
  await vm.shot('files');await vm.key('ctrl','w');await vm.stop();await vm.start();assert.equal((await fileBytes(vm,'/home/user/Documents/Alpha.txt')).toString(),'original-content');await waitFor(()=>exists('/home/user/Documents/Folder'),'file committed');pass('Data survives a cold reboot');
  if(!legacy){
   await open('/home/user/Documents');let w=await files(vm);await select('Large.bin');await vm.key('ctrl','c');await vm.key('ctrl','v');await waitFor(async()=>!!await exists('/home/user/Documents/Copy of Large.bin'),'large copy');assert((await fileBytes(vm,'/home/user/Documents/Large.bin')).equals(large));assert((await fileBytes(vm,'/home/user/Documents/Copy of Large.bin')).equals(large));pass('Exact 64-KiB binary copy, no truncation');
   await select('Copy of Large.bin');await menu(6);await vm.type('Large.bin');await vm.key('ret');await expectNotice(vm);await vm.key('esc');assert((await fileBytes(vm,'/home/user/Documents/Large.bin')).equals(large));assert((await fileBytes(vm,'/home/user/Documents/Copy of Large.bin')).equals(large));pass('Rename conflict preserves both files');
   await vm.key('ctrl','w');await open('/boot');w=await files(vm);await select('readme.txt');await vm.key('delete');await expectNotice(vm);await vm.key('esc');await select('readme.txt');await vm.key('ctrl','c');await open('/home/user/Documents');await files(vm);await vm.key('ctrl','v');await waitFor(async()=>!!await exists('/home/user/Documents/readme.txt'),'readonly source copied home');assert((await fileBytes(vm,'/home/user/Documents/readme.txt')).equals(Buffer.from(fs.readFileSync(path.join(__dirname,'../disk32/README.TXT'),'utf8').replace(/\r?\n/g,'\r\n'))));pass('Read-only source may be copied, never deleted');await vm.key('ctrl','w');await vm.key('ctrl','w');
   await open('/Applications');w=await files(vm);await vm.double(...await rowPoint(vm,w,'qa-calc.lnk'));await calculator(vm);await vm.type('6*7=');await expectDisplay(vm,await calculator(vm),'42.00');await vm.key('ctrl','w');await vm.key('ctrl','w');pass('Merged application library launches a real installed alias');
   await vm.key('f2');let start=vm.log().length;await vm.type('run /boot/files.snx /selftest\n');await waitFor(async()=>vm.log().slice(start).includes('FILES SELFTEST PASS'),'real Files syscall/capability selftest');pass('Real user ABI selftest: invalid pointers, rights, bounds and denied capabilities');await waitFor(async()=>!(await vm.state()).windows.some(w=>w.app===9&&w.title!=='SINO Shell'),'selftest process closes');await vm.key('ctrl','w');
   await waitFor(async()=>(await vm.state()).windows.length===0,'selftest exits and closes');const pages=await vm.value('free_pages');
   for(let i=0;i<4;i++){await vm.key('f1');await files(vm);await vm.key('ctrl','w');await waitFor(async()=>await vm.value('free_pages')===pages,'Files pages reclaimed');}pass('Repeated close reclaims process, scene, handles and drag registrations');
   await vm.key('f6');const cw=await require('./clock-test').clock(vm);await vm.key('f2');const faults=await vm.value('process_faults');await vm.type('run /boot/files.snx /fault-test\n');await waitFor(async()=>await vm.value('process_faults')===faults+1,'real Files fault isolated');await waitFor(async()=>(await vm.state()).windows.filter(w=>w.app===9&&w.title!=='SINO Shell').length===1,'fault window reclaimed');await vm.key('ctrl','w');assert((await vm.state()).windows.some(w=>w.id===cw.id));const before=await require('./clock-test').clockTime(vm,cw);await waitFor(async()=>await require('./clock-test').clockTime(vm,cw)!==before,'clock survives Files fault');await vm.key('ctrl','w');await waitFor(async()=>await vm.value('free_pages')===pages,'fault resources reclaimed');pass('Actual Files fault is isolated; clock advances and resources recover');
   for(let i=0;i<8;i++){await vm.key('f4');await calculator(vm);}
   await vm.key('f1');assert.equal((await vm.state()).modal,2);assert(!(await vm.state()).windows.some(w=>w.app===1));await vm.key('ret');for(let i=0;i<8;i++)await vm.key('ctrl','w');await waitFor(async()=>await vm.value('free_pages')===pages,'full table reclaimed');pass('Full window table fails visibly with no Ring 0 fallback');

   await vm.stop();await vm.start();assert((await fileBytes(vm,'/home/user/Documents/Large.bin')).equals(large));assert((await fileBytes(vm,'/home/user/Documents/Copy of Large.bin')).equals(large));pass('Binary copies survive another real reboot');
  }
  fs.writeFileSync(path.join(vm.run,'report.json'),JSON.stringify({result:'PASS',legacy,checks},null,2));
 }catch(e){try{await vm.shot('failure');}catch{}fs.writeFileSync(path.join(vm.run,'report.json'),JSON.stringify({result:'FAIL',legacy,checks,error:e.stack,log:vm.log()},null,2));throw e;}finally{await vm.stop();}
})().catch(e=>{console.error(e);process.exitCode=1;});
