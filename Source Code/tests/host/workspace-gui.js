const assert=require('node:assert/strict');
async function run(api){
 const {check,launch,processes,resources,until,key,type,text,waitText,command}=api;
 const baseline=await resources();let pid;
 async function run(line,code=0){await type(line+'\n');await until(async()=>(await text(pid)).endsWith('gsh:/ $'),'write command prompt');await type('status\n');await until(async()=>{const s=await text(pid);return s.slice(s.lastIndexOf('gsh:/ $ status')).includes('status code='+code+' reason=1 start=0');},'exact write command status');}
 await check('G5b explicit readonly session refuses write requests',async()=>{
  pid=await launch('term','');await waitText(pid,'gsh:/ $');await run('touch g5b-denied',1);await waitText(pid,'read-only session');
  await key('ctrl','w');await until(async()=>(await processes()).length===0,'readonly cleanup');assert.deepEqual(await resources(),baseline);
 });
 await check('G5b strict workspace file lifecycle, append, copy and Busy protection',async()=>{
  pid=await launch('term','/write');await waitText(pid,'/ = /home/user (read-write)');
  await run('mkdir g5b');await run('touch g5b/empty');await run('write g5b/a alpha beta');await run('append g5b/a gamma');
  await run('cp g5b/a g5b/b');assert.equal(await command('cat /home/user/g5b/b'),'alpha beta\ngamma\n');
  await run('mv g5b/b g5b/c');await run('mv g5b/c g5b/a',1);await run('cp g5b/a g5b/a',1);
  await run('write ../g5b-outside denied',1);await run('rm welcome.txt/invalid',1);
  await run('rm g5b/a');await run('rm g5b/c');await run('rm g5b/empty');
  const second=await launch('term','');await waitText(second,'gsh:/ $');await type('cd g5b\n');await waitText(second,'gsh:/g5b $');
  await key('alt','tab');await run('rmdir g5b',1);await key('alt','tab');await key('ctrl','w');await run('rmdir g5b');await run('sync');
  await key('ctrl','w');await until(async()=>(await processes()).length===0,'workspace cleanup');assert.deepEqual(await resources(),baseline);
 });
 await check('G5b copy cancellation and size failure discard private stages',async()=>{
  await launch('rdtest','fixtures');await until(async()=>(await processes()).length===0,'large write fixtures');const held=await resources();
  pid=await launch('term','/write');await waitText(pid,'/ = /home/user (read-write)');
  await type('cp Documents/g4/big.txt g5b-cancel.bin\n');
  await until(async()=>(await processes()).some(p=>p.name==='writer.snx'),'copy worker running');await key('ctrl','c');
  await waitText(pid,'task ended reason=3');await until(async()=>(await text(pid)).endsWith('gsh:/ $'),'canceled copy restores Shell');
  await command('stat /home/user/g5b-cancel.bin',1);
  await run('append Documents/g4/big.txt excess',1);
  assert.equal(await command('wc -c /home/user/Documents/g4/big.txt'),'65536\n');
  await run('cp Documents/g4/binary.txt g5b-copy.bin');
  assert.equal(await command('hexdump /home/user/g5b-copy.bin'),'00000000  41 00 1b 42                                      |A..B|\n');
  await run('rm g5b-copy.bin');
  await key('ctrl','w');await until(async()=>(await processes()).length===0,'large copy cleanup');assert.deepEqual(await resources(),held);
  await launch('rdtest','cleanup');await until(async()=>(await processes()).length===0,'large fixtures removed');assert.deepEqual(await resources(),baseline);
 });

}
module.exports={run};

async function persist(api){
 const {check}=api;
 await check('GUI Shell commits its own cold-boot fixture',async()=>{
 const {launch,until,processes,type,text,key,waitText}=api;
 const pid=await launch('term','/write');await waitText(pid,'/ = /home/user (read-write)');
 for(const line of ['write g5b-durable.txt g5b durable','sync']){await type(line+'\n');await until(async()=>(await text(pid)).endsWith('gsh:/ $'),'durable write prompt');}
 await waitText(pid,'home snapshot durable');await key('ctrl','w');await until(async()=>(await processes()).length===0,'durable Shell close');
 });
}
module.exports.persist=persist;
