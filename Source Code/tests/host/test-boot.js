const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {spawn} = require('node:child_process');
const {createHash} = require('node:crypto');
const {setTimeout: sleep} = require('node:timers/promises');
const {qemuPath} = require('../../tools/dev-run');
const {bootPayload} = require('../support/iso-boot');

const hash = bytes => createHash('sha256').update(bytes).digest('hex');

async function boot(executable, directory, buildId) {
    let firmware = path.join(path.dirname(executable), 'share');
    if (fs.existsSync(path.join(firmware, 'qemu', 'bios-256k.bin')))
        firmware = path.join(firmware, 'qemu');
    const disk = fs.openSync(path.join(directory, 'data.img'), 'wx');
    try { fs.ftruncateSync(disk, 16 * 1024 * 1024); }
    finally { fs.closeSync(disk); }

    const kernelLog = path.join(directory, 'kernel.log');
    const stderr = fs.openSync(path.join(directory, 'qemu-stderr.log'), 'wx');
    let child;
    try {
        child = spawn(executable, [
            '-L', firmware, '-machine', 'pc,vmport=on', '-accel', 'tcg', '-m', '64M',
            '-vga', 'std', '-nic', 'none', '-display', 'none', '-monitor', 'none',
            '-serial', 'none', '-no-reboot',
            '-drive', 'file=SINO.iso,format=raw,media=cdrom,if=ide,index=2,readonly=on',
            '-drive', 'file=data.img,format=raw,if=ide,index=0', '-boot', 'order=d',
            '-debugcon', 'file:kernel.log', '-global', 'isa-debugcon.iobase=0xe9'
        ], {cwd:directory, windowsHide:true, stdio:['ignore', 'ignore', stderr]});
    } finally { fs.closeSync(stderr); }

    let spawnError;
    child.on('error', error => { spawnError = error; });
    const stopped = new Promise(resolve => child.once('close', resolve));
    const deadline = Date.now() + 30000;
    try {
        while (true) {
            if (spawnError) throw spawnError;
            const log = fs.existsSync(kernelLog) ? fs.readFileSync(kernelLog, 'utf8') : '';
            assert(!log.includes('KERNEL PANIC'), 'Kernel panic during boot');
            assert(child.exitCode === null && child.signalCode === null, 'QEMU exited during boot');
            if (log.includes('SINO CLI ready\n')) {
                assert(log.includes(`SINO BUILD ID: ${buildId}\n`), 'Guest build ID mismatch');
                assert(log.includes('FAT12 RAM disk ready'), 'FAT12 volume did not mount');
                assert(log.includes('TIMER IRQ0 delivered'), 'Timer interrupt did not arrive');
                assert(!log.includes('SINO SHELL BRIDGE'), 'Expected an ordinary build without ShellBridge');
                return;
            }
            assert(Date.now() < deadline, 'Boot timed out');
            await sleep(50);
        }
    } finally {
        if (child.pid && child.exitCode === null && child.signalCode === null) child.kill();
        await stopped;
    }
}

async function main(args) {
    if (args.length === 1 && args[0] === '--list') { console.log('boot.ordinary  ISO identity, FAT12, timer and ordinary boot (1 case)'); return; }
    assert.equal(args.length, 1, 'Usage: node tests/host/test-boot.js <BuildOnly run directory>');
    const directory = path.resolve(args[0]);
    const build = JSON.parse(fs.readFileSync(path.join(directory, 'build.json'), 'utf8'));
    assert(/^[A-Za-z0-9._-]{1,64}$/.test(build.buildId || ''), 'Invalid build ID');
    const iso = fs.readFileSync(path.join(directory, 'SINO.iso'));
    assert.equal(hash(iso), build.isoSha256, 'ISO does not match the build record');
    assert.equal(hash(bootPayload(iso)), build.bootImageSha256, 'ISO boot payload does not match the build record');
    console.log(`PASS ISO9660 / El Torito boot payload for ${build.buildId}`);

    const executable = qemuPath();
    const run = fs.mkdtempSync(path.join(directory, 'boot-'));
    fs.writeFileSync(path.join(run, 'SINO.iso'), iso, {flag:'wx'});
    console.log(`Boot logs: ${run}`);
    await boot(executable, run, build.buildId);
    fs.writeFileSync(path.join(run, 'boot-report.json'), JSON.stringify({result:'PASS',
        buildId:build.buildId, cases:[{id:'boot.ordinary', result:'PASS'}]}, null, 2));
    console.log(`PASS headless CD boot for ${build.buildId} with FAT12, timer IRQ0 and ShellBridge disabled`);
}

main(process.argv.slice(2)).catch(error => {
    console.error(error.message);
    process.exitCode = 1;
});
