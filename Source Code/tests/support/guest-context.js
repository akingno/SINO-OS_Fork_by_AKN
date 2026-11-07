/* Shared guest transport and inspection only; no suite dispatch or business cases. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {execFileSync} = require('node:child_process');
const host = require('../../tools/host-tools');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');

function createGuest(context) {
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
    const objectHeader = fs.readFileSync(path.join(__dirname, '../../src32/object.h'), 'utf8');
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

    return {directory, build, job, qmp, channel, report, guestLog, check, command,
        resources, value, until, launch, processes, gone, native, memory, symbols};
}
module.exports = {createGuest};
