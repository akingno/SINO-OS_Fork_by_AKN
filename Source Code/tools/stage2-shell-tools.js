const assert=require('node:assert/strict');
async function run(api){
 const {check,launch,processes,resources,until,key,type,text,waitText,command}=api;
 const baseline=await resources();let pid;
 async function complete(line){await type(line+'\n');await until(async()=>/gsh:\/[^\n]* \$$/.test(await text(pid)),'G5a prompt');}
 async function row(line){await until(async()=>(await text(pid)).split('\n').includes(line),line);}
 await check('G5a user banner, 120-column output, command help and aliases',async()=>{
  pid=await launch('term','/g3');await waitText(pid,'SINO user Shell');
  await waitText(pid,'120 x 25');await waitText(pid,'/ = /home/user (read-only)');
  await complete('echo '+ 'w'.repeat(120));await row('w'.repeat(120));
  await complete('help grep');await waitText(pid,'grep [-n] text path');
  await complete('which pwd');await row('pwd: builtin');
  await complete('which cat');await row('cat: /boot/tview.snx (catalog 5)');
  await complete('which DIR');await row('ls: /boot/tview.snx (catalog 5)');
  await complete('which install');await row('which: unavailable command');
  await complete('help noSuchCommand');await row('help: unavailable command');
  await complete("echo -e 'a\\nb'");await row('a');await row('b');
  await complete("echo -e '\\x1b'");await row('echo: unsupported escape');
  await key('ctrl','w');await until(async()=>(await processes()).length===0,'G5a banner cleanup');assert.deepEqual(await resources(),baseline);
 });
 await check('G5a explicit head line count uses the same existing welcome file',async()=>{
  pid=await launch('term','/g3');await waitText(pid,'gsh:/ $');
  async function fresh(line){await complete(line);const screen=await text(pid),marker='gsh:/ $ '+line;const at=screen.lastIndexOf(marker);assert(at>=0,'current command echo');return screen.slice(at+marker.length).trim();}
  const output=await fresh('head -n 2 welcome.txt');
  assert.equal(output,'Welcome to SINO Platinum Desktop.\ngsh:/ $','first line plus blank second line');
  const status=await fresh('status');assert.equal(status,'status code=0 reason=1 start=0\ngsh:/ $');
  const missing=await fresh('head -n 2 nonexistent-welcome.txt');assert.equal(missing,'file error -4\ngsh:/ $');
  assert.equal(await fresh('status'),'status code=1 reason=1 start=0\ngsh:/ $');
  assert((await fresh('head welcome.txt')).includes('Use the SINO menu to launch applications.'));
  await key('ctrl','w');await until(async()=>(await processes()).length===0,'head reproduction cleanup');assert.deepEqual(await resources(),baseline);
 });
 await check('G5a public information commands report real values',async()=>{
  pid=await launch('term','/g3');await waitText(pid,'gsh:/ $');
  await complete('ver');await row('SINO 0.10 SINO32 i386');
  await complete('uname -a');await row('SINO 0.10 SINO32 i386');
  await complete('time');assert.match(await text(pid),/\b\d{2}:\d{2}:\d{2}\b/);
  await complete('date');assert.match(await text(pid),/\b\d{4}-\d{2}-\d{2}\b/);
  await complete('uptime');assert.match(await text(pid),/up \d+ seconds/);
  await complete('credits');await waitText(pid,'GNU Unifont');
  await complete('mem');await row('unavailable: mem');
  await key('ctrl','w');await until(async()=>(await processes()).length===0,'G5a info cleanup');assert.deepEqual(await resources(),baseline);
 });
 await check('G5a text tools handle options, raw counts and bounded long lines',async()=>{
  await launch('rdtest','fixtures');await until(async()=>(await processes()).length===0,'G5a fixtures');
  const held=await resources();pid=await launch('term','/g3');await waitText(pid,'gsh:/ $');
  await complete('head -n 1 Documents/g4/pages.txt');await row('page line');
  await complete('tail -n 1 Documents/g4/pages.txt');await row('PAGE-END');
  await complete('wc Documents/g4/tiny.txt');await row('0 1 5');
  await complete('wc -c Documents/g4/big.txt');await row('65536');
  await complete('grep -n END Documents/g4/pages.txt');await row('31:PAGE-END');
  await complete('grep absent Documents/g4/tiny.txt');await type('status\n');await row('status code=1 reason=1 start=0');
  await complete('head -n -1 Documents/g4/tiny.txt');await waitText(pid,'invalid line count');
  await complete('grep END Documents/g4/big.txt');await waitText(pid,'G4-END');
  await complete('hexdump Documents/g4/binary.txt');await waitText(pid,'41 00 1b 42');
  await complete('find Documents/g4 tiny');await row('/Documents/g4/tiny.txt');
  await complete('tree Documents/g4');await waitText(pid,'tiny.txt');
  await key('ctrl','w');await until(async()=>(await processes()).length===0,'G5a tools cleanup');assert.deepEqual(await resources(),held);
  await launch('rdtest','cleanup');await until(async()=>(await processes()).length===0,'G5a remove fixtures');assert.deepEqual(await resources(),baseline);
  assert.equal(await command('echo g5a-compatible'),'g5a-compatible\n');
 });
}
module.exports={run};
