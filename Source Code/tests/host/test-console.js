/* Real VGA text and PS/2 input; no desktop, scene, screenshot or mouse helpers. */
const fs = require('node:fs'), path = require('node:path'), net = require('node:net');
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const {createHash} = require('node:crypto');
const root = path.resolve(__dirname, '../..');
const {QMP} = require(path.join(root, 'tools/qmp'));
const {qemuPath, sources} = require(path.join(root, 'tools/dev-run'));
const vm = require('../../tools/vm-config');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const persistence = process.argv.includes('--persistence');
const {volume} = require(path.join(root, 'tools/test-volume'));
const cases = persistence ? ['console.persist-create', 'console.persist-update', 'console.persist-delete'] : ['console.autostart', 'console.edit-history', 'console.workspace',
    'console.cancel', 'console.ascii-input', 'console.restart', 'console.scrollback', 'console.scroll-input', 'console.scroll-output'];
async function main(arg) {
    if (arg === '--list') { console.log(cases.join('\n') + '\n' + cases.length + ' cases'); return; }
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
    let child, stopped, error, q;
    function launch() {
    error = null;
    const stderr = fs.openSync(path.join(directory,'qemu-stderr.log'),'a');
    child = spawn(exe,['-L',firmware,'-machine','pc','-accel','tcg',...vm.args(),'-vga','std',
        '-nic','none','-display','none','-serial','none','-monitor','none','-no-reboot',
        '-drive','file=SINO.iso,format=raw,media=cdrom,if=ide,index=2,readonly=on',
        '-drive','file=data.img,format=raw,if=ide,index=0','-boot','order=d',
        '-debugcon','file:kernel.log','-global','isa-debugcon.iobase=0xe9',
        '-qmp',`tcp:127.0.0.1:${port},server=on,wait=off`],{cwd:directory,windowsHide:true,stdio:['ignore','ignore',stderr]});
    fs.closeSync(stderr);
    child.on('error',e=>error=e);
    stopped = new Promise(resolve=>child.once('close',resolve));
    }
    async function stop() {
        if(q){q.close();q=null;}
        if(child&&child.pid&&child.exitCode===null)child.kill();
        if(stopped)await stopped;
    }
    launch();
    const report={result:'FAIL',buildId:build.buildId,cases:[]};
    const log=()=>fs.existsSync(path.join(directory,'kernel.log'))?fs.readFileSync(path.join(directory,'kernel.log'),'utf8'):'';
    async function until(fn,label,timeout=20000){const end=Date.now()+timeout;while(true){
        if(error)throw error;assert(child.exitCode===null && !/KERNEL PANIC|USER PROBE FAIL/.test(log()),'guest failed');
        if(await fn())return;assert(Date.now()<end,'Timeout: '+label);await sleep(50);
    }}
    async function screen(){const file=path.join(directory,'vga.bin');await q.hmp(`pmemsave 0xb8000 4000 "${file.replaceAll('\\','/')}"`);
        const b=fs.readFileSync(file);assert(Array.from({length:2000},(_,i)=>b[i*2+1]).every(a=>a===7),'monochrome VGA');return Array.from({length:25},(_,y)=>Array.from({length:80},(_,x)=>String.fromCharCode(b[(y*80+x)*2])).join('').trimEnd()).join('\n').trimEnd();}
    async function key(...codes){await q.call('send-key',{keys:codes.map(data=>({type:'qcode',data})),'hold-time':15});await sleep(65);}
    async function type(text){const codes={' ':'spc','\n':'ret','.':'dot','/':'slash','-':'minus',';':'semicolon'};
        for(const c of text){if(c==='>')await key('shift','dot');else await key(codes[c]||c);}}
    async function command(text,expected){await type(text+'\n');await until(async()=>{const s=await screen();return s.includes(expected)&&s.endsWith('gsh:/ $');},text);}
    async function check(id,fn){const item={id,result:'FAIL'};report.cases.push(item);await fn();item.result='PASS';console.log('PASS '+id);}
    try {
        await until(async()=>{try{q=await new QMP(port).connect();return true;}catch{return false;}},'QMP');
        report.profile = await vm.verify(q);
        if (persistence) {
            const snapshot = () => volume(path.join(directory,'data.img')).current.records;
            async function reboot() {
                await stop(); launch();
                await until(async()=>{try{q=await new QMP(port).connect();return true;}catch{return false;}},'reconnect');
                await until(()=>log().includes('CONSOLE SHELL ready'),'Shell restart');
                await until(async()=>(await screen()).endsWith('gsh:/ $'),'restarted prompt');
            }
            await until(()=>log().includes('CONSOLE SHELL ready'),'Shell ready');
            await check(cases[0],async()=>{
                await command('mkdir persist-dir','');
                await command('echo alpha > persist-dir/file.txt','');
                await command('touch kept.txt','');
                assert.equal(snapshot().get('/home/user/persist-dir/file.txt')?.data.toString(),'alpha\n');
                assert(snapshot().has('/home/user/kept.txt'));
                await reboot();await command('cat persist-dir/file.txt','\nalpha\n');
            });
            await check(cases[1],async()=>{
                await command('echo beta >> persist-dir/file.txt','');
                await command('mv persist-dir/file.txt persist-dir/moved.txt','');
                const saved=snapshot();assert(!saved.has('/home/user/persist-dir/file.txt'));
                assert.equal(saved.get('/home/user/persist-dir/moved.txt')?.data.toString(),'alpha\nbeta\n');
                await reboot();await command('cat persist-dir/moved.txt','\nalpha\nbeta\n');
            });
            await check(cases[2],async()=>{
                await command('rm persist-dir/moved.txt','');await command('rmdir persist-dir','');
                assert(![...snapshot().keys()].some(p=>p==='/home/user/persist-dir'||p.startsWith('/home/user/persist-dir/')));
                await reboot();await command('ls','kept.txt');
                const saved=snapshot();assert(saved.has('/home/user/kept.txt'));
                assert(![...saved.keys()].some(p=>p==='/home/user/persist-dir'||p.startsWith('/home/user/persist-dir/')));
            });
        } else {
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
            await until(async()=>{const s=await screen();return !s.includes('nihao')&&s.endsWith('gsh:/ $');},'new prompt');await command('echo restarted','\nrestarted\n');});
        await check(cases[6],async()=>{
            // A long wrapped command plus its output exercises the same circular history.
            await command('echo '+ 'abcdefghij'.repeat(16),'abcdefghij');
            await command('help','');
            const live = await screen();
            await key('shift','pgup');
            await until(async()=>{const s=await screen();return s!==live&&s.split('\n')[24]==='gsh:/ $';},'pinned prompt');
            const older=await screen();
            await sleep(200);assert.equal(await screen(),older,'history stays still while idle');
            await key('shift','pgdn');
            await until(async()=>(await screen())===live,'return to live output');
        });
        await check(cases[7],async()=>{
            await type('echo draft');await key('shift','pgup');
            await until(async()=>(await screen()).split('\n')[24]==='gsh:/ $ echo draft','pinned draft');
            await key('left');await type('x');await key('ret');
            await until(async()=>{const s=await screen();return s.includes('\ndrafxt\n')&&s.endsWith('gsh:/ $');},'resume draft editing');
        });
        await check(cases[8],async()=>{
            await type('sleep 3\n');await sleep(300);
            await key('shift','pgup');
            const before=(await screen()).split('\n').slice(0,24).join('\n');
            await sleep(3400);
            await until(async()=>{const lines=(await screen()).split('\n');
                return lines[24]==='gsh:/ $'&&lines.slice(0,24).join('\n')===before;
            },'new output keeps history anchored');
            await key('shift','pgdn');
            await until(async()=>{const s=await screen();return s.includes('gsh:/ $ sleep 3\n')&&s.endsWith('gsh:/ $');},'completed command retained');
        });
        }
        assert.equal(sources().sha256,build.sourceSha256,'source unchanged during tests');report.result='PASS';
    } catch(e){report.error=e.stack;throw e;}
    finally{await stop();
        fs.writeFileSync(path.join(directory,'console-report.json'),JSON.stringify(report,null,2));console.log('Report: '+directory);}
}
main(process.argv[2]).catch(e=>{console.error(e);process.exitCode=1;});
