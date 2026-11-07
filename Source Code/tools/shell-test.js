/* Headless Shell execution through COM1. All guest disks are private to this
   invocation. QMP is used only to verify attachments and inspect regressions. */
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const crypto = require('node:crypto');
const {spawn} = require('node:child_process');
const {QMP} = require('./qmp');
const {qemuPath, sources} = require('./dev-run');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const readJSON = file => JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');

function bounded(promise, milliseconds, label) {
    let timer;
    return Promise.race([promise, new Promise((_, reject) => {
        timer = setTimeout(() => reject(Error(label)), milliseconds);
    })]).finally(() => clearTimeout(timer));
}

class ShellChannel {
    constructor(socket) {
        this.socket = socket;
        this.buffer = Buffer.alloc(0);
        this.pending = null;
        this.failure = null;
        socket.on('data', bytes => this.receive(bytes));
        socket.on('error', error => this.fail(error));
        socket.on('close', () => this.fail(Error('Shell channel closed.')));
    }
    fail(error) {
        if (!this.failure) this.failure = error;
        this.buffer = Buffer.alloc(0);
        if (this.pending) {
            this.pending.reject(error);
            this.pending = null;
        }
        this.socket.destroy();
    }
    receive(bytes) {
        if (this.failure) return;
        this.buffer = Buffer.concat([this.buffer, bytes]);
        if (this.buffer.length > 8212) return this.fail(Error('Shell response exceeds its bound.'));
        if (this.buffer.length < 20) return;
        const header = this.buffer;
        const size = header.readUInt32LE(16);
        if (!this.pending || header.toString('ascii', 0, 4) !== this.pending.replyMagic || size > 8192 ||
            header.readUInt32LE(4) !== this.pending.id || header.readUInt32LE(12) > 1) {
            return this.fail(Error('Invalid Shell response frame or request identity.'));
        }
        if (this.buffer.length < 20 + size) return;
        if (this.buffer.length !== 20 + size) return this.fail(Error('Unexpected bytes after Shell response.'));
        const result = {status:header.readInt32LE(8), overflow:!!header.readUInt32LE(12),
            output:Buffer.from(this.buffer.subarray(20))};
        this.buffer = Buffer.alloc(0);
        const pending = this.pending;
        this.pending = null;
        pending.resolve(result);
    }
    async request(id, command, timeoutMs, options = {}) {
        if (this.failure) throw this.failure;
        if (this.pending) throw Error('Only one Shell request may be outstanding.');
        const bytes = Buffer.from(command, 'utf8');
        const frame = Buffer.alloc(12 + bytes.length);
        const magic = options.magic || 'SCS1';
        if (!['SCS1', 'SCS2'].includes(magic)) throw Error('Unknown transport version.');
        frame.write(magic);
        frame.writeUInt32LE(id, 4);
        frame.writeUInt32LE(options.declaredSize ?? bytes.length, 8);
        bytes.copy(frame, 12);
        const response = new Promise((resolve, reject) => {
            this.pending = {id, resolve, reject, replyMagic:magic === 'SCS1' ? 'SCR1' : 'SCR2'};
        });
        const sent = options.prefix ? Buffer.concat([Buffer.from(options.prefix), frame]) : frame;
        if (options.fragment) {
            this.socket.write(sent.subarray(0, 7));
            await sleep(30);
            this.socket.write(sent.subarray(7));
        } else this.socket.write(sent);
        try {return await bounded(response, timeoutMs, 'Shell command timed out.');}
        catch (error) {this.fail(error);throw error;}
    }
    close() {this.socket.destroy();}
}

function scenarios(job) {
    if (!job.selfTest) return [{command:job.command, status:job.expectedExitCode}];
    if (job.suite === 'commands') return require('../tests/support/shell-command-cases');
    return require('../tests/support/shell-transport-cases');
}

async function run(directory, {execute, seedDisk, memoryMiB = 64} = {}) {
    const job = readJSON(path.join(directory, 'shell-job.json'));
    const build = readJSON(path.join(directory, 'build.json'));
    const report = {version:1, build, result:'FAIL', stage:'identity', cases:[],
        outputMode:'combined Shell output; diagnostics separately in kernel.log',
        memoryMiB, startedAt:new Date().toISOString()};
    const reportFile = path.join(directory, 'shell-report.json');
    const dataFile = path.join(directory, 'shell-data.img');
    const kernelLog = path.join(directory, 'kernel.log');
    let child, channel, qmp, server, childError;
    const guestLog = () => fs.existsSync(kernelLog) ? fs.readFileSync(kernelLog, 'utf8') : '';
    try {
        if (![64, 128, 1024].includes(memoryMiB)) throw Error('Unsupported test VM memory size.');
        if (sources().sha256 !== build.sourceSha256 || hash(build.iso) !== build.isoSha256 ||
            path.resolve(build.iso) !== path.join(directory, 'SINO.iso')) throw Error('Build inputs or ISO identity changed.');
        if (seedDisk) {
            if (!fs.statSync(seedDisk).isFile() || fs.statSync(seedDisk).size !== 16 * 1024 * 1024)
                throw Error('Unexpected private seed disk.');
            fs.copyFileSync(seedDisk, dataFile, fs.constants.COPYFILE_EXCL);
            report.seedDisk = {path:seedDisk, sha256:hash(seedDisk)};
        } else {
            const disk = fs.openSync(dataFile, 'wx');
            try {fs.ftruncateSync(disk, 16 * 1024 * 1024);} finally {fs.closeSync(disk);}
        }
        report.privateDisk = dataFile;
        report.stage = 'boot';
        let accept;
        const connected = new Promise(resolve => {accept = resolve;});
        server = net.createServer(socket => {
            if (channel) return socket.destroy();
            channel = new ShellChannel(socket);
            accept(channel);
        });
        await new Promise((resolve, reject) => {
            server.once('error', reject);
            server.listen(0, '127.0.0.1', resolve);
        });
        const serialPort = server.address().port;
        const exe = qemuPath();
        let firmware = path.join(path.dirname(exe), 'share');
        if (fs.existsSync(path.join(firmware, 'qemu', 'bios-256k.bin'))) firmware = path.join(firmware, 'qemu');
        const args = ['-L', firmware, '-name', 'SINO Shell '+build.buildId,
            '-machine','pc,vmport=on','-accel','tcg','-m',`${memoryMiB}M`,'-vga','std','-nic','none',
            '-display','none','-monitor','none','-rtc','base=localtime',
            '-drive','file=SINO.iso,format=raw,media=cdrom,if=ide,index=2,readonly=on',
            '-drive','file=shell-data.img,format=raw,if=ide,index=0','-boot','order=d',
            '-serial',`tcp:127.0.0.1:${serialPort}`,
            '-debugcon','file:kernel.log','-global','isa-debugcon.iobase=0xe9'];
        // QMP uses a second short-lived loopback listener to reserve its port.
        const reservation = net.createServer();
        await new Promise((resolve, reject) => {reservation.once('error', reject);reservation.listen(0,'127.0.0.1',resolve);});
        const qmpPort = reservation.address().port;
        await new Promise(resolve => reservation.close(resolve));
        args.push('-qmp', `tcp:127.0.0.1:${qmpPort},server=on,wait=off`);
        const stderr = fs.openSync(path.join(directory, 'qemu-stderr.log'), 'w');
        try {child = spawn(exe, args, {cwd:directory, windowsHide:true, stdio:['ignore','ignore',stderr]});}
        finally {fs.closeSync(stderr);}
        child.on('error', error => {childError = error;});
        report.qemu = {executable:exe, pid:child.pid};
        const deadline = Date.now() + 30000;
        while (!guestLog().includes('SINO desktop ready')) {
            if (childError) throw childError;
            if (child.exitCode !== null) throw Error('QEMU exited before boot. See qemu-stderr.log.');
            if (guestLog().includes('KERNEL PANIC')) throw Error('Kernel panic during boot.');
            if (Date.now() > deadline) throw Error('Boot timed out.');
            await sleep(50);
        }
        if (!guestLog().includes('SINO BUILD ID: '+build.buildId+'\n') ||
            !guestLog().includes('SINO SHELL BRIDGE v1 ready')) throw Error('Wrong guest build or Shell bridge disabled.');
        await bounded(connected, 2000, 'COM1 connection timed out.');
        qmp = new QMP(qmpPort);
        await bounded(qmp.connect(), 3000, 'QMP connection timed out.');
        const blocks = (await qmp.call('query-block')).filter(b => b.inserted).map(b => ({
            file:path.resolve(directory,b.inserted.file), readonly:b.inserted.ro}));
        if (blocks.length !== 2 || !blocks.some(b => b.file === build.iso && b.readonly) ||
            !blocks.some(b => b.file === dataFile && !b.readonly)) throw Error('Unexpected test disk attachments.');
        report.mounted = blocks;
        report.stage = 'commands';
        if (execute) await execute({directory, build, job, qmp, channel, report, guestLog});
        let id = 0;
        for (const scenario of execute ? [] : scenarios(job)) {
            const item = {id:scenario.id || 'shell.single', requestId:++id, command:scenario.command,
                expected:{status:scenario.status, output:scenario.output, contains:scenario.contains, pattern:scenario.pattern, overflow:!!scenario.overflow}, result:'FAIL'};
            report.cases.push(item);
            const start = Date.now();
            const actual = await channel.request(id, scenario.command, job.timeoutMs, scenario);
            item.milliseconds = Date.now()-start;
            item.actual = {status:actual.status, overflow:actual.overflow, bytes:actual.output.length};
            item.outputFile = `command-${id}.out`;
            fs.writeFileSync(path.join(directory,item.outputFile),actual.output);
            item.actual.output = actual.output.toString('utf8');
            if (actual.status !== scenario.status || actual.overflow !== !!scenario.overflow ||
                (scenario.output !== undefined && !actual.output.equals(Buffer.from(scenario.output))) ||
                (scenario.contains !== undefined && !item.actual.output.includes(scenario.contains)) ||
                (scenario.pattern !== undefined && !new RegExp(scenario.pattern).test(item.actual.output))) {
                item.error = `Request ${id} failed: expected ${JSON.stringify(item.expected)}, actual ${JSON.stringify(item.actual)}`;
                if (job.suite !== 'commands') throw Error(item.error);
                console.error(item.error);
                continue;
            }
            item.result = 'PASS';
            console.log(`PASS request ${id}: ${JSON.stringify(scenario.command)} (exit ${actual.status})`);
            if (!job.selfTest) process.stdout.write(actual.output);
        }
        if (guestLog().includes('KERNEL PANIC')) throw Error('Guest kernel panic.');
        if (!report.cases.length) throw Error('No test cases executed.');
        if (sources().sha256 !== build.sourceSha256 || hash(build.iso) !== build.isoSha256)
            throw Error('Build inputs or ISO changed during the test.');
        report.result = report.cases.every(item => item.result === 'PASS') ? 'PASS' : 'FAIL';
        report.stage = 'complete';
    } catch (error) {
        report.error = error.message;
        console.error(error.message);
        if (qmp) {
            try {
                const registers = await qmp.hmp('info registers');
                fs.writeFileSync(path.join(directory,'failure-registers.txt'),registers);
                if (job.gui) await qmp.call('screendump',{filename:'failure.ppm'});
            } catch (diagnosticError) {report.diagnosticError=diagnosticError.message;}
        }
    } finally {
        if (channel) channel.close();
        if (qmp) qmp.close();
        if (server) server.close();
        if (child && child.pid && child.exitCode === null) {
            const exited = new Promise(resolve => child.once('exit', resolve));
            child.kill();
            try {await bounded(exited, 3000, 'Test QEMU did not exit.');}
            catch (error) {report.cleanupError=error.message;report.result='FAIL';}
        }
        report.finishedAt = new Date().toISOString();
        report.vmStopped = !child || !child.pid || child.exitCode !== null || child.signalCode !== null;
        fs.writeFileSync(reportFile, JSON.stringify(report,null,2)+'\n');
        console.log(`${report.result}: ${reportFile}`);
    }
    return report.result === 'PASS' ? 0 : 1;
}

if (require.main === module) {
    const directory = path.resolve(process.argv[2] || '');
    run(directory).then(code => {process.exitCode=code;}).catch(error => {console.error(error);process.exitCode=1;});
}
module.exports = {ShellChannel, run};
