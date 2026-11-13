/* Local development only: boot a newly built ISO with a separate data disk.
   No release disk updates, guest package upgrades, or physical disk access. */
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const net = require('node:net');
const {spawn, execFileSync} = require('node:child_process');
const {QMP} = require('./qmp');
const host = require('./host-tools');
const vm = require('./vm-config');
const sourceRoot = path.resolve(__dirname, '..');
const repoRoot = path.dirname(sourceRoot);
const devRoot = path.join(repoRoot, 'Build Output', 'dev');
const currentFile = path.join(devRoot, 'current-run.json');
const dataFile = path.join(devRoot, 'data.img');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const sha = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const readJSON = file => JSON.parse(fs.readFileSync(file, 'utf8'));
const writeJSON = (file, value) => fs.writeFileSync(file, JSON.stringify(value, null, 2) + '\n');

function runPath(value) {
    const directory = path.resolve(value || '');
    const relative = path.relative(path.join(devRoot, 'runs'), directory);
    if (!relative || relative.startsWith('..') || path.isAbsolute(relative)) throw Error('Expected a private development run directory.');
    return directory;
}

function sources() {
    const files = [];
    function walk(directory) {
        for (const item of fs.readdirSync(directory, {withFileTypes:true}).sort((a,b) => a.name.localeCompare(b.name))) {
            const file = path.join(directory, item.name);
            if (item.isDirectory()) walk(file);
            else if (item.isFile()) files.push(file);
        }
    }
    for (const name of ['src32','user','disk32','resources','tools','tests']) walk(path.join(sourceRoot, name));
    for (const name of fs.readdirSync(sourceRoot)) if (/^build.*\.(js|ps1)$/.test(name)) files.push(path.join(sourceRoot, name));
    files.push(path.join(sourceRoot, 'LICENSE'));
    files.push(path.join(sourceRoot, 'release/launch.ps1'));
    files.push(path.join(repoRoot, 'dev.ps1'));
    files.push(path.join(repoRoot, 'run.ps1'));
    const inputs = files.sort().map(file => ({path:path.relative(repoRoot,file).replaceAll('\\','/'),sha256:sha(fs.readFileSync(file))}));
    return {sha256:sha(Buffer.from(JSON.stringify(inputs))),inputs};
}

async function connect(port, timeout=2000) {
    const qmp = new QMP(port);
    let timer;
    try {
        return await Promise.race([qmp.connect(),new Promise((_,reject) => {
            timer = setTimeout(() => reject(Error('QMP greeting timed out; VM status is unknown.')),timeout);
        })]);
    } catch (error) { qmp.close(); throw error; }
    finally { clearTimeout(timer); }
}

async function check() {
    if (!fs.existsSync(currentFile)) return;
    const prior = readJSON(currentFile);
    if (prior.running === false) return;
    let qmp;
    try { qmp = await connect(prior.qmpPort); }
    catch (error) { if (error.code === 'ECONNREFUSED') return; throw error; }
    try {
        const name = await qmp.call('query-name');
        throw Error(`A VM is still using the recorded development endpoint (${name.name}). Close its window before starting a new run.`);
    } finally { qmp.close(); }
}

function record(directory) {
    const prepared = readJSON(path.join(directory,'prepare.json'));
    const now = sources();
    if (now.sha256 !== prepared.source.sha256) throw Error('Source inputs changed during the build. Rebuild before booting.');
    const image = fs.readFileSync(path.join(sourceRoot,'sino.img'));
    const iso = fs.readFileSync(path.join(directory,'SINO.iso'));
    const manifest = readJSON(path.join(sourceRoot,'build32','manifest-normal.json'));
    if (manifest.sha256 !== sha(image)) throw Error('Boot-image manifest mismatch.');
    const catalog = iso.readUInt32LE(17*2048+71);
    const bootSector = iso.readUInt32LE(catalog*2048+40);
    if (!iso.subarray(bootSector*2048,bootSector*2048+image.length).equals(image)) throw Error('ISO does not contain the just-built boot image.');
    const header = fs.readFileSync(path.join(sourceRoot,'build32','build-id.h'),'ascii');
    if (!header.includes(`"${prepared.buildId}"`)) throw Error('Build ID header mismatch.');
    const build = {
        buildId:prepared.buildId,sourceCommit:prepared.sourceCommit,sourceSha256:now.sha256,
        iso:path.join(directory,'SINO.iso'),isoSha256:sha(iso),bootImageSha256:sha(image),
        kernelSha256:sha(fs.readFileSync(path.join(sourceRoot,'build32','kernel-normal.bin'))),
        kernelBytes:manifest.kernelBytes,bssEnd:manifest.bssEnd,node:process.version,
        createdAt:new Date().toISOString()
    };
    writeJSON(path.join(directory,'boot-manifest.json'),manifest);
    writeJSON(path.join(directory,'build.json'),build);
    return build;
}

function qemuPath() {
    const folders = [process.env.SINO_QEMU_DIR,path.join(process.env.ProgramFiles || 'C:\\Program Files','qemu')].filter(Boolean);
    for (const name of ['qemu-system-i386w.exe','qemu-system-i386.exe']) {
        try { folders.push(path.dirname(execFileSync('where.exe',[name],{encoding:'utf8',windowsHide:true,stdio:['ignore','pipe','ignore']}).trim().split(/\r?\n/)[0])); } catch {}
    }
    for (const folder of folders) for (const name of ['qemu-system-i386w.exe','qemu-system-i386.exe']) {
        const file = path.join(folder,name);
        if (fs.existsSync(file)) return file;
    }
    throw Error('QEMU not found. Install it or set SINO_QEMU_DIR.');
}

async function availablePort() {
    const server = net.createServer();
    await new Promise((resolve,reject) => {server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});
    const port = server.address().port;
    await new Promise(resolve => server.close(resolve));
    return port;
}

async function run(directory) {
    await check();
    const build = record(directory);
    const exe = qemuPath();
    let firmware = path.join(path.dirname(exe),'share');
    if (fs.existsSync(path.join(firmware,'qemu','bios-256k.bin'))) firmware = path.join(firmware,'qemu');
    if (!fs.existsSync(dataFile)) {
        const fd = fs.openSync(dataFile,'wx');
        try {fs.ftruncateSync(fd,16*1024*1024);} finally {fs.closeSync(fd);}
    }
    if (!fs.statSync(dataFile).isFile() || fs.statSync(dataFile).size !== 16*1024*1024) throw Error('Unexpected development data disk.');
    const port = await availablePort();
    const name = `SINO Dev ${build.buildId}`;
    const args = ['-L',firmware,'-name',name,'-machine','pc,vmport=on','-accel','tcg',...vm.args(),'-vga','std','-nic','none',
        '-drive','file=SINO.iso,format=raw,media=cdrom,if=ide,index=2,readonly=on',
        '-drive','file=../../data.img,format=raw,if=ide,index=0','-boot','order=d','-rtc','base=localtime',
        '-display','gtk,show-menubar=off,show-cursor=off,zoom-to-fit=off',
        '-debugcon','file:kernel.log','-global','isa-debugcon.iobase=0xe9','-qmp',`tcp:127.0.0.1:${port},server=on,wait=off`];
    const logFile = path.join(directory,'kernel.log');
    const stderr = fs.openSync(path.join(directory,'startup.log'),'w');
    let child;
    try {child = spawn(exe,args,{cwd:directory,detached:true,windowsHide:false,stdio:['ignore','ignore',stderr]});}
    finally {fs.closeSync(stderr);}
    let spawnError;
    child.on('error',error => {spawnError=error;});
    child.unref();
    const state = {buildId:build.buildId,runDirectory:directory,qmpPort:port,pid:child.pid,running:true};
    writeJSON(currentFile,state);
    let qmp;
    try {
        const deadline = Date.now()+30000;
        for (;;) {
            if (spawnError) throw spawnError;
            if (child.exitCode !== null) throw Error('QEMU exited: '+fs.readFileSync(path.join(directory,'startup.log'),'utf8'));
            const log = fs.existsSync(logFile)?fs.readFileSync(logFile,'utf8'):'';
            if (log.includes('KERNEL PANIC')) throw Error('Guest kernel panic. See '+logFile);
            if (log.includes(`SINO BUILD ID: ${build.buildId}\n`) && log.includes('SINO CLI ready')) break;
            if (Date.now()>deadline) throw Error('Guest did not confirm this build ID and CLI readiness. See '+logFile);
            await sleep(100);
        }
        qmp = await connect(port);
        if ((await qmp.call('query-name')).name !== name) throw Error('Unexpected QMP VM identity.');
        const profile = await vm.verify(qmp);
        const status = await qmp.call('query-status');
        if (!status.running) throw Error('Guest is not running.');
        const blocks = await qmp.call('query-block');
        const mounted = blocks.filter(b=>b.inserted).map(b=>({file:path.resolve(directory,b.inserted.file),readonly:b.inserted.ro}));
        if (!mounted.some(b=>b.file===build.iso&&b.readonly)) throw Error('The built ISO is not mounted read-only.');
        if (mounted.length!==2 || !mounted.some(b=>b.file===dataFile)) throw Error('Unexpected VM disk attachment.');
        if (sha(fs.readFileSync(build.iso))!==build.isoSha256) throw Error('ISO changed after build verification.');
        const checks = ['source fingerprint unchanged during build','ISO embeds current boot image','guest reports matching build ID','CLI ready without kernel panic','QMP reports running','only development ISO and data disk attached'];
        const report = {...build,result:'PASS',verifiedAt:new Date().toISOString(),qemu:exe,qmpPort:port,pid:child.pid,profile,mounted,checks};
        writeJSON(path.join(directory,'verification.json'),report);
        console.log(`PASS ${build.buildId}\nISO: ${build.iso}\nSHA256: ${build.isoSha256}\nReport: ${path.join(directory,'verification.json')}\nDevelopment VM is running. Close its window before the next development launch.`);
    } catch (error) {
        child.kill();
        state.running=false;
        writeJSON(currentFile,state);
        writeJSON(path.join(directory,'verification.json'),{...build,result:'FAIL',error:error.message});
        throw error;
    } finally {if(qmp)qmp.close();}
}

module.exports = {qemuPath, sources};

if (require.main === module) (async()=>{
    const [command,value,buildId] = process.argv.slice(2);
    if (command==='check') return check();
    const directory = runPath(value);
    if (command==='prepare') {
        execFileSync(host.python(), [path.join(sourceRoot,'tools/build-locale.py')], {windowsHide:true,stdio:'inherit'});
        if (!/^[A-Za-z0-9._-]{1,64}$/.test(buildId || '')) throw Error('Invalid build ID.');
        fs.mkdirSync(directory,{recursive:true});
        let commit='unknown';
        try {commit=execFileSync('git',['rev-parse','HEAD'],{cwd:repoRoot,encoding:'utf8',windowsHide:true}).trim();} catch {}
        writeJSON(path.join(directory,'prepare.json'),{buildId,sourceCommit:commit,source:sources()});
        console.log('Build directory: '+directory);
    } else if (command==='record') {
        console.log(JSON.stringify(record(directory),null,2));
    } else if (command==='run') {
        if (process.argv.length !== 4) throw Error('Use dev.ps1 -VerifyOnly for headless Ring 3 checks.');
        await run(directory);
    } else throw Error('Use check, prepare <run-directory> <build-id>, record <run-directory>, or run <run-directory>.');
})().catch(error=>{console.error(error.message);process.exitCode=1;});
