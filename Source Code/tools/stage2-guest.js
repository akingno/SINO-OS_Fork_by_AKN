/* Text assertions against actual Ring 3 clients. No pixel or coordinate tests. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {execFileSync} = require('node:child_process');
const host = require('./host-tools');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');

async function run(context, mode = 'native') {
    const {directory, build, job, qmp, channel, report, guestLog} = context;
    const symbolFile = path.join(path.dirname(directory), 'kernel-normal.exe');
    assert.equal(hash(symbolFile), build.symbolsSha256, 'matching symbol snapshot');
    const symbols = {};
    for (const line of execFileSync(host.tool('nm'), ['-n', symbolFile],
        {encoding:'utf8', windowsHide:true}).split('\n')) {
        const match = line.match(/^([0-9a-f]+) [A-Za-z] _(\w+)\s*$/);
        if (match) symbols[match[2]] = parseInt(match[1], 16);
    }
    // Fail visibly when this private inspection adapter needs updating.
    const objectHeader = fs.readFileSync(path.join(__dirname, '../src32/object.h'), 'utf8');
    assert.match(objectHeader, /u32\s+generation,\s*refs,\s*type;\s*int\s+value;\s*}\s*Object;/);
    const objectCount = Number(objectHeader.match(/#define\s+OBJECTS\s+(\d+)/)?.[1]);
    assert(objectCount > 0 && objectCount <= 1024, 'object-table adapter bound');
    report.inspection = {symbolsSha256:build.symbolsSha256, objectCount, objectBytes:16};
    report.commands = [];
    let requestId = 0;
    function healthy() {
        const log = guestLog();
        assert(!/KERNEL PANIC|USER PROBE FAIL:|SELFTEST FAIL|NETWORK USER FAIL:/.test(log),
            'Guest reported failure: ' + log.slice(-1800));
    }
    async function until(predicate, label, timeout = job.timeoutMs) {
        const deadline = Date.now() + timeout;
        for (;;) {
            healthy();
            if (await predicate()) return;
            assert(Date.now() < deadline, 'Timed out: ' + label);
            await sleep(25);
        }
    }
    async function command(text, status = 0) {
        const id = ++requestId;
        const actual = await channel.request(id, text, job.timeoutMs);
        const output = actual.output.toString('utf8');
        const file = `command-${id}.out`;
        fs.writeFileSync(path.join(directory, file), actual.output);
        report.commands.push({id, command:text, expectedStatus:status,
            actualStatus:actual.status, overflow:actual.overflow, outputFile:file});
        assert.equal(actual.status, status, text + ': ' + output);
        assert.equal(actual.overflow, false, text + ': output overflow');
        return output;
    }
    async function check(name, action) {
        const item = {id:report.cases.length + 1, name, result:'FAIL'};
        report.cases.push(item);
        const started = Date.now();
        try {
            await action(item);
            healthy();
            item.result = 'PASS';
            console.log('PASS ' + name);
        } catch (error) { item.error = error.message; throw error; }
        finally { item.milliseconds = Date.now() - started; }
    }
    async function memory(address, bytes) {
        assert(Number.isInteger(address), 'missing symbol address');
        await qmp.hmp(`pmemsave 0x${address.toString(16)} ${bytes} "inspection.bin"`);
        const value = fs.readFileSync(path.join(directory, 'inspection.bin'));
        assert.equal(value.length, bytes);
        return value;
    }
    const value = async name => (await memory(symbols[name], 4)).readUInt32LE();
    async function resources() {
        const objects = await memory(symbols.objects, objectCount * 16);
        let refs = 0, live = 0;
        for (let i = 0; i < objectCount; i++) {
            const n = objects.readUInt32LE(i * 16 + 4);
            refs += n;
            if (n) live++;
        }
        return {freePages:await value('free_pages'), objectReferences:refs, liveObjects:live};
    }
    async function processes() {
        return (await command('ps')).split(/\r?\n/).flatMap(line => {
            const m = line.match(/^(\d+)\s+(\d+)\s+(\d+)\s+(\S+)$/);
            return m ? [{pid:Number(m[1]), threads:Number(m[2]), pages:Number(m[3]), name:m[4]}] : [];
        });
    }
    async function gone(pid) {
        await until(async () => !(await processes()).some(p => p.pid === pid), 'process reaped: ' + pid);
    }
    async function launch(name, args = '') {
        const output = await command(`run /boot/${name}.snx${args ? ' ' + args : ''}`);
        const match = output.match(/^Started user process (\d+)\n$/);
        assert(match, 'launch response: ' + output);
        return Number(match[1]);
    }
    async function native(name, args, marker) {
        const offset = guestLog().length;
        const faults = await value('process_faults');
        const exits = await value('process_exits');
        const pid = await launch(name, args);
        await until(() => guestLog().slice(offset).includes(marker), marker);
        await gone(pid);
        assert.equal(await value('process_faults'), faults, name + ': unexpected fault');
        assert.equal(await value('process_exits'), exits + 1, name + ': normal exit observed');
        return guestLog().slice(offset);
    }

    if (mode === 'reject') {
        await check('intentional guest failure must reject the gate', async () => {
            await launch('probe', '/stage2-fail');
            // Log is written by several syscalls; wait for the complete reason
            // before asking the normal failure detector to reject it.
            const deadline = Date.now() + job.timeoutMs;
            while (!guestLog().includes('USER PROBE FAIL: intentional Stage 2 runner failure\n')) {
                assert(!guestLog().includes('KERNEL PANIC'), 'panic is not the expected probe failure');
                assert(Date.now() < deadline, 'intentional failure marker timed out');
                await sleep(25);
            }
            await until(() => false, 'intentional probe cannot pass');
        });
        throw Error('The intentional failure was incorrectly accepted.');
    }
    if (mode === 'reboot') {
        await check('cold boot completes package journal recovery', async () => {
            await until(async () => (await processes()).length === 0, 'boot recovery exits', 20000);
            assert.match(guestLog(), /STORAGE restored generation/);
            await native('instest', 'recover', 'INSTALL CRASH RECOVERY PASS');
        });
        await check('cold boot preserves exact binary bytes', async () => {
            await native('probe', '/stage2-read', 'STAGE2 PERSISTENCE PASS');
        });
        await check('cold boot preserves Shell file contents', async () => {
            assert.equal(await command('cat /home/user/stage2-marker.txt'), 'stage2 durable marker\n');
            assert.equal((await processes()).length, 0);
        });
        return;
    }

    await check('boot and kernel object contracts', async () => {
        assert.match(guestLog(), /PAGE allocator self-check passed/);
        assert.match(guestLog(), /OBJECT handles rights\/stale\/release PASS/);
        assert.match(guestLog(), /CPL3 selectors 1B\/23, I\/O denied/);
        assert.equal((await processes()).length, 0);
    });
    let baseline;
    await check('headless Ring 3 ABI, permissions, threads, handles and binary VFS', async item => {
        const before = await resources();
        const output = await native('probe', '/stage2', 'STAGE2 PROBE PASS checks=');
        const count = Number(output.match(/STAGE2 PROBE PASS checks=(\d+)/)?.[1]);
        assert(count >= 70, 'incomplete headless assertions');
        item.guestAssertions = count;
        baseline = await resources();
        assert.deepEqual(baseline, {...before, freePages:before.freePages - 1},
            'only the 257-byte persistent binary retains one page');
        report.resourceBaseline = baseline;
    });
    for (let i = 1; i <= 8; i++) {
        await check(`normal exit resource recovery ${i}/8`, async () => {
            await native('probe', '/stage2', 'STAGE2 PROBE PASS checks=');
            assert.deepEqual(await resources(), baseline);
        });
    }
    for (const [app, marker] of [
        ['calc', 'CALCULATOR SELFTEST PASS'], ['clock', 'CLOCK SELFTEST PASS'],
        ['files', 'FILES SELFTEST PASS'], ['bstest', 'BASIC SELFTEST PASS']
    ]) {
        await check(`${app} existing user application contracts`, async () => {
            await native(app, app === 'bstest' ? '' : '/selftest', marker);
            assert.deepEqual(await resources(), baseline);
        });
    }
    for (const [app, vector] of [['fault', 14], ['iofault', 13], ['udfault', 6]]) {
        await check(`${app} fault is contained and resources are reclaimed`, async () => {
            const before = await value('process_faults');
            const offset = guestLog().length;
            const pid = await launch(app);
            await until(async () => await value('process_faults') === before + 1, app + ' fault');
            await gone(pid);
            assert(guestLog().slice(offset).includes(' vector=0x' + vector.toString(16).toUpperCase().padStart(8, '0')),
                'expected fault vector: ' + guestLog().slice(offset));
            assert.deepEqual(await resources(), baseline);
        });
    }
    await check('user preemption permits a second process and Shell progress', async () => {
        const pid = await launch('spin');
        try {
            const ticks = await value('ticks');
            await native('probe', '/stage2', 'STAGE2 PROBE PASS checks=');
            assert((await processes()).some(p => p.pid === pid));
            assert((await value('ticks')) !== ticks);
            assert.equal(await command('echo responsive'), 'responsive\n');
        } finally { await command('kill ' + pid); await gone(pid); }
        assert.deepEqual(await resources(), baseline);
    });
    await check('full process table rejects launch and cancellation releases every slot', async () => {
        const pids = [];
        try {
            for (let i = 0; i < 8; i++) pids.push(await launch('spin'));
            assert.equal((await processes()).length, 8);
            assert.match(await command('run /boot/spin.snx', 1), /native launch failed/);
        } finally {
            for (const pid of pids) await command('kill ' + pid);
            for (const pid of pids) await gone(pid);
        }
        assert.deepEqual(await resources(), baseline);
    });
    await check('loopback network runs in Ring 3 and releases its handles', async () => {
        const offset = guestLog().length;
        const before = await resources();
        const faults = await value('process_faults');
        await command('nettest');
        await until(() => guestLog().slice(offset).includes('NETWORK USER TEST PASS'), 'loopback network', 20000);
        const list = (await processes()).filter(p => p.name === 'nettest.snx');
        assert.equal(list.length, 1);
        await command('kill ' + list[0].pid);
        await gone(list[0].pid);
        assert.equal(await value('process_faults'), faults);
        assert.deepEqual(await resources(), before);
    });
    await check('package validation, rollback, install, repair and uninstall', async () => {
        await native('instest', '', 'INSTALL SELFTEST PASS');
        assert.equal((await processes()).length, 0);
    });
    await check('prepare committed binary, Shell marker and interrupted install for reboot', async () => {
        assert.equal(await command('echo stage2 durable marker > /home/user/stage2-marker.txt'), '');
        const offset = guestLog().length;
        const faults = await value('process_faults');
        const pid = await launch('instest', 'crash');
        await until(() => guestLog().slice(offset).includes('INSTALL CRASH JOURNAL READY'), 'crash journal');
        await gone(pid);
        assert.equal(await value('process_faults'), faults + 1);
        assert.match(await command('sync'), /Home data saved/);
        assert.equal((await processes()).length, 0);
        report.finalResources = await resources();
    });
}

module.exports = {run};
