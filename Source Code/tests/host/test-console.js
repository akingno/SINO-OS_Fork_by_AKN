/* Real VGA text and PS/2 input; no desktop, scene, screenshot or mouse helpers. */
const fs = require('node:fs'), path = require('node:path'), net = require('node:net');
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const {createHash} = require('node:crypto');
const root = path.resolve(__dirname, '../..');
const {QMP} = require(path.join(root, 'tools/qmp'));
const {qemuPath, sources} = require(path.join(root, 'tools/dev-run'));
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const cases = ['console.autostart', 'console.edit-history', 'console.workspace',
    'console.cancel', 'console.ascii-input', 'console.restart'];
async function main(arg) {
    if (arg === '--list') { console.log(cases.join('\n') + '\n6 cases'); return; }
    assert(arg, 'Pass an ordinary BuildOnly directory');
    const buildDir = path.resolve(arg), build = JSON.parse(fs.readFileSync(path.join(buildDir,'build.json')));
    assert.equal(sources().sha256, build.sourceSha256, 'current source identity');
    const iso = fs.readFileSync(path.join(buildDir,'SINO.iso'));
    assert.equal(createHash('sha256').update(iso).digest('hex'), build.isoSha256);
    const directory = fs.mkdtempSync(path.join(buildDir,'console-'));
    fs.writeFileSync(path.join(directory,'SINO.iso'), iso);
    const disk = fs.openSync(path.join(directory,'data.img'),'wx'); fs.ftruncateSync(disk,16*1024*1024);fs.closeSync(disk);
    const server = net.createServer();await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
    const port = server.address().port;await new Promise(resolve=>server.close(resolve));
    const exe = qemuPath();let firmware=path.join(path.dirname(exe),'share');
    if(fs.existsSync(path.join(firmware,'qemu','bios-256k.bin'))) firmware=path.join(firmware,'qemu');
    const stderr = fs.openSync(path.join(directory,'qemu-stderr.log'),'wx');
    const child = spawn(exe,['-L',firmware,'-machine','pc','-accel','tcg','-m','64M','-vga','std',
        '-nic','none','-display','none','-serial','none','-monitor','none','-no-reboot',
        '-drive','file=SINO.iso,format=raw,media=cdrom,if=ide,index=2,readonly=on',
        '-drive','file=data.img,format=raw,if=ide,index=0','-boot','order=d',
        '-debugcon','file:kernel.log','-global','isa-debugcon.iobase=0xe9',
        '-qmp',`tcp:127.0.0.1:${port},server=on,wait=off`],{cwd:directory,windowsHide:true,stdio:['ignore','ignore',stderr]});
    fs.closeSync(stderr);
    let error;child.on('error',e=>error=e);
    const stopped = new Promise(resolve=>child.once('close',resolve));
    const report={result:'FAIL',buildId:build.buildId,cases:[]};let q;
    const log=()=>fs.existsSync(path.join(directory,'kernel.log'))?fs.readFileSync(path.join(directory,'kernel.log'),'utf8'):'';
    async function until(fn,label,timeout=20000){const end=Date.now()+timeout;while(true){
        if(error)throw error;assert(child.exitCode===null && !/KERNEL PANIC|USER PROBE FAIL/.test(log()),'guest failed');
        if(await fn())return;assert(Date.now()<end,'Timeout: '+label);await sleep(50);
    }}
    async function screen(){const file=path.join(directory,'vga.bin');await q.hmp(`pmemsave 0xb8000 4000 "${file.replaceAll('\\','/')}"`);
        const b=fs.readFileSync(file);return Array.from({length:25},(_,y)=>Array.from({length:80},(_,x)=>String.fromCharCode(b[(y*80+x)*2])).join('').trimEnd()).join('\n').trimEnd();}
    async function key(...codes){await q.call('send-key',{keys:codes.map(data=>({type:'qcode',data})),'hold-time':15});await sleep(65);}
    async function type(text){const codes={' ':'spc','\n':'ret','.':'dot','/':'slash','-':'minus',';':'semicolon'};
        for(const c of text){if(c==='>')await key('shift','dot');else await key(codes[c]||c);}}
    async function command(text,expected){await type(text+'\n');await until(async()=>{const s=await screen();return s.includes(expected)&&s.endsWith('gsh:/ $');},text);}
    async function check(id,fn){const item={id,result:'FAIL'};report.cases.push(item);await fn();item.result='PASS';console.log('PASS '+id);}
    try {
        await until(async()=>{try{q=await new QMP(port).connect();return true;}catch{return false;}},'QMP');
        await check(cases[0],async()=>{await until(()=>log().includes('CONSOLE SHELL ready'),'autostart');
            assert(log().includes(`SINO BUILD ID: ${build.buildId}`));assert(!log().includes('SINO SHELL BRIDGE'));
            await until(async()=>(await screen()).endsWith('gsh:/ $'),'visible prompt');});
        await check(cases[1],async()=>{await type('echo helo');await key('left');await type('l');await key('ret');
            await until(async()=>/\nhello\n/.test(await screen()),'cursor edit');await key('up');await key('ret');
            await until(async()=>((await screen()).match(/\nhello\n/g)||[]).length===2,'history');});
        await check(cases[2],async()=>{await command('echo saved > console.txt','');
            await command('cat console.txt','\nsaved\n');await command('sync','');});
        await check(cases[3],async()=>{await type('sleep 30\n');await sleep(300);await key('ctrl','c');
            await until(async()=>(await screen()).endsWith('gsh:/ $'),'cancel');await command('echo resumed','\nresumed\n');});
        await check(cases[4],async()=>{await key('ctrl','spc');await command('echo nihao','\nnihao\n');});
        await check(cases[5],async()=>{const before=log().length;await key('ctrl','d');
            await until(()=>log().slice(before).includes('CONSOLE SHELL restarting'),'restart');
            await until(async()=>(await screen()).endsWith('gsh:/ $'),'new prompt');await command('echo restarted','\nrestarted\n');});
        assert.equal(sources().sha256,build.sourceSha256,'source unchanged during tests');report.result='PASS';
    } catch(e){report.error=e.stack;throw e;}
    finally{if(q)q.close();if(child.pid&&child.exitCode===null)child.kill();await stopped;
        fs.writeFileSync(path.join(directory,'console-report.json'),JSON.stringify(report,null,2));console.log('Report: '+directory);}
}
main(process.argv[2]).catch(e=>{console.error(e);process.exitCode=1;});
