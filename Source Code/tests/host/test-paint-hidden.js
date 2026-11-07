const {files:filesWindow}=require('../support/files-test');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {VM,sleep}=require('../../tools/vm-test'),{waitFor,scene}=require('../support/calculator-test');
const {seedPaintDisk,nodes,fileBytes}=require('../support/paint-fixture'),{volume}=require('../../tools/test-volume');
const legacy=process.argv.includes('--legacy');
(async()=>{const vm=new VM(legacy?'paint-baseline':'paint-hidden-qa'),checks=[];const pass=s=>{checks.push(s);console.log('PASS '+s);};const expected=seedPaintDisk(path.join(vm.run,'data.img'));
 const open=async name=>{await vm.key('ctrl','o');await vm.type(name);await vm.key('ret');};
 const checkFiles=async()=>{for(const [name,e] of expected)if(!e.dir)assert((await fileBytes(vm,name)).equals(e.data),name);};
 try{
  assert(volume(path.join(vm.run,'data.img')).current.records.get('/home/user/Desktop/Sketch.snp').data.equals(expected.get('/home/user/Desktop/Sketch.snp').data));
  await vm.start();await checkFiles();pass('Existing drawing and legacy alias fixtures restore byte-for-byte');
  if(legacy){
   for(const route of ['f5','menu','/Applications/Paint.app','/home/user/Desktop/Paint.app','/home/user/Desktop/Sketch.snp']){
    if(route==='f5')await vm.key('f5');else if(route==='menu'){await vm.click(22,13);await vm.click(46,160);}else await open(route);
    await waitFor(async()=>(await vm.state()).windows.find(w=>w.app===4),'legacy Paint '+route);await vm.shot('legacy-'+checks.length);await vm.key('ctrl','w');pass('Legacy Paint reachable: '+route);
   }
  }else{
   const noPaint=async()=>assert(!(await vm.state()).windows.some(w=>w.app===4),'Paint must never create a window');
   const notice=async name=>{await open(name);assert.equal((await vm.state()).modal,2,name);assert.match((await vm.memory(vm.symbols.message,128)).toString().split('\0')[0],/画板已暂停|Paint is unavailable/);await noPaint();await vm.key('esc');};
   assert(!(await nodes(vm)).some(n=>n.path==='/Applications/Paint.app'));pass('Built-in Paint.app is absent');
   for(let i=0;i<12;i++)await vm.key('f5');assert.equal((await vm.state()).windows.length,0);assert.equal((await vm.state()).modal,0);pass('Repeated global F5 cannot launch Paint');
   const all=await nodes(vm),desk=await vm.memory(vm.symbols.desk_nodes,32*4),visible=Array.from({length:await vm.value('desk_count')},(_,i)=>all.find(n=>n.id===desk.readInt32LE(i*4))?.name);
   assert(visible.includes('Sketch.snp'));assert(!visible.includes('Paint.app'));assert(!visible.includes('OldPaint.lnk'));await vm.shot('01-hidden-desktop');pass('Desktop hides obsolete aliases while keeping drawings visible');
   await vm.click(22,13);await vm.shot('02-menu');await vm.click(46,160);await waitFor(async()=>(await vm.state()).windows.some(w=>w.app===9&&w.title==='SINO Shell'),'Ring 3 Terminal menu');await noPaint();await vm.key('ctrl','w');pass('Menu rows after removal still launch Terminal');
   for(const route of ['/home/user/Desktop/Paint.app','/home/user/Desktop/OldPaint.lnk','/home/user/.sino/menu/OldPaint.lnk','/home/user/Desktop/Sketch.snp','/home/user/Documents/CAPS.SNP'])await notice(route);
   await open('/Applications/Paint.app');await noPaint();assert.equal((await vm.state()).windows.length,0);await vm.key('esc');pass('Old app, shortcut, missing built-in and case-insensitive drawing routes cannot launch Paint');
   const sketch=all.find(n=>n.name==='Sketch.snp');await vm.double((await vm.memory(vm.symbols.desk_x+sketch.id*4,4)).readInt32LE()+45,(await vm.memory(vm.symbols.desk_y+sketch.id*4,4)).readInt32LE()+20);assert.equal((await vm.state()).modal,2);await noPaint();await vm.key('esc');pass('Double-clicking a real desktop drawing gives the paused notice');
   await open('/Applications');let w=await filesWindow(vm),seen=[];
   for(let i=0;i<20;i++){await vm.key('down');w=await filesWindow(vm);const name=all.find(n=>n.id===w.selected)?.name;if(name)seen.push(name);}
   assert(seen.includes('Calculator.app'));assert(!seen.includes('Paint.app'));assert(!seen.includes('OldPaint.lnk'));await vm.key('ctrl','w');pass('Application library excludes legacy aliases and retains other applications');
   await vm.key('f2');let installed=0;for(const app of ['editor','files']){await vm.type(`install /quiet:/boot/${app}.sxp\n`);await waitFor(async()=>(vm.log().match(/QUIET INSTALL PASS/g)||[]).length>installed,'install '+app);installed++;}
   await vm.type('run /home/user/Programs/files/files.snx /home/user/Desktop\n');w=await filesWindow(vm);
   let commands=await scene(vm,w);assert(!commands.some(c=>c.text==='Paint.app'||c.text==='OldPaint.lnk'));const row=commands.find(c=>c.text==='Sketch.snp');assert(row);await vm.click(w.x+200,w.y+row.y+8);await vm.key('ret');
   assert.equal((await vm.state()).modal,2);assert.match((await vm.memory(vm.symbols.message,128)).toString(),/画板已暂停/);await vm.key('esc');assert(!(await vm.state()).windows.some(w=>w.title.includes('原生文本编辑器')));await noPaint();await vm.shot('03-native-files-paused');await vm.key('ctrl','w');await vm.key('ctrl','w');pass('Native Files blocks the text-editor fallback even with Editor installed');
   await vm.key('f7');w=(await vm.state()).windows.find(w=>w.app===7);await vm.click(w.x+50,w.y+196);await vm.click(w.x+515,w.y+160);await vm.click(w.x+w.w-86,w.y+w.h-34);w=(await vm.state()).windows.find(w=>w.app===7);await vm.click(w.x+w.w-86,w.y+w.h-49);await waitFor(async()=>await vm.value('settings_pending')===0,'keep language');assert.equal(await vm.value('settings_language'),1);await vm.key('ctrl','w');await open('/home/user/Desktop/Sketch.snp');assert.equal((await vm.state()).modal,2);assert.match((await vm.memory(vm.symbols.message,128)).toString(),/Paint is unavailable/);await vm.shot('04-english-paused');await vm.key('esc');pass('Paused notice follows Chinese and English UI language');
  }
  await checkFiles();await vm.stop();for(const [name,e] of expected)if(!e.dir)assert(volume(path.join(vm.run,'data.img')).current.records.get(name).data.equals(e.data));
  await vm.start();await checkFiles();await vm.stop();pass('Drawings and aliases persist through a cold reboot');
  fs.writeFileSync(path.join(vm.run,'report.json'),JSON.stringify({result:'PASS',legacy,checks},null,2));
 }catch(e){try{await vm.shot('failure');}catch{}fs.writeFileSync(path.join(vm.run,'report.json'),JSON.stringify({result:'FAIL',legacy,checks,error:e.stack,log:vm.log()},null,2));throw e;}finally{await vm.stop();}
})().catch(e=>{console.error(e);process.exitCode=1;});
