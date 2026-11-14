const assert = require('node:assert/strict');
const cases = require('../../support/pid-cases.json');
const fields = ['processes', 'abortedProcesses', 'objects', 'handles', 'threads',
    'processLimits', 'objectLimits', 'handleLimits', 'threadLimits'];
async function run(api) {
    const {check, command, launch, gone, processes, resources, until, guestLog,
        memory, symbols, value, report} = api;
    report.identifierAudit = [];
    async function stats() {
        const bytes = await memory(symbols.identifier_stats, 56);
        return Object.fromEntries(fields.map((field, i) => [field,
            i < 5 ? Number(bytes.readBigUInt64LE(i * 8)) : bytes.readUInt32LE(40 + (i - 5) * 4)]));
    }
    async function measured(label, action) {
        const before = await stats();
        await action();
        const after = await stats();
        const delta = Object.fromEntries(fields.map(field => [field, after[field] - before[field]]));
        report.identifierAudit.push({label, before, after, delta});
        return delta;
    }
    async function probe(mode) {
        const offset = guestLog().length, faults = await value('process_faults');
        const pid = await launch('probe', '/pid-' + mode);
        await until(() => guestLog().slice(offset).includes('IDENTIFIER PROBE PASS /pid-' + mode), mode);
        await gone(pid);
        assert.equal(await value('process_faults'), faults);
    }
    for (const item of cases) {
        await check(item.name, async () => {
            const baseline = await resources();
            if (item.name === 'reuse') {
                assert.match(guestLog(), /IDENTIFIERS object\/cookie exhaustion PASS/);
                let old = 0;
                const seen = new Set();
                for (let i = 0; i < 12; i++) {
                    const delta = await measured('untracked-launch-kill', async () => {
                        const pid = await launch('spin');
                        assert(pid > 0 && pid <= 0x7fffffff && (pid & 15) === 1 && !seen.has(pid));
                        if (old) {
                            assert.match(await command('kill ' + old, 1), /no such process/);
                            assert((await processes()).some(p => p.pid === pid), 'stale kill leaves new process');
                        }
                        seen.add(pid);
                        await command('kill ' + pid); await gone(pid); old = pid;
                    });
                    assert.equal(delta.processes, 1);
                    assert.equal(delta.objects + delta.handles + delta.threads, 0);
                }
            } else if (item.name === 'capacity') {
                const pids = [];
                try {
                    for (let i = 0; i < 8; i++) { pids.push(await launch('spin')); }
                    assert.equal(new Set(pids.map(pid => pid & 15)).size, 8);
                    const before = await stats();
                    await command('run /boot/spin.snx', 1);
                    assert.equal((await stats()).processes, before.processes);
                    for (const bad of [0, 1, 16, 0x7fffffff]) { await command('kill ' + bad, 1); }
                    assert.equal((await processes()).length, 8);
                } finally {
                    for (const pid of pids) { await command('kill ' + pid); await gone(pid); }
                }
            } else if (item.name === 'rollback') {
                const previous = await launch('spin');
                await command('kill ' + previous); await gone(previous);
                const offset = guestLog().length;
                const delta = await measured('failed-image-after-eight-pages', () =>
                    command('run -s /boot/memv2.snx oom-image', 1));
                assert.match(guestLog().slice(offset), /MM PROBE injected stage=0x00000001/);
                assert.equal(delta.processes, 0);
                assert.equal(delta.abortedProcesses, 1);
                assert.deepEqual(await resources(), baseline);
                const next = await launch('spin');
                assert.equal(next, previous + 16, 'failed reservation does not consume generation');
                await command('kill ' + next); await gone(next);
            } else {
                if (item.name === 'shell-file') {
                    const shell = await measured('gsh-session-and-external-echo', async () => {
                        const offset = guestLog().length;
                        const pid = await launch('brokerqa', 'shell-smoke');
                        await until(() => guestLog().slice(offset).includes('SHELL PASS'), 'gsh echo');
                        await gone(pid);
                        await until(async () => (await processes()).length === 0, 'Shell session cleanup');
                    });
                    assert.equal(shell.processes, 3, 'broker, real gsh and strict echo worker');
                }
                const delta = await measured(item.name, () => probe(item.name));
                const expected = {
                    history:{processes:3, objects:2, handles:2, threads:0},
                    'shell-file':{processes:1, objects:2, handles:2, threads:0},
                    pipe:{processes:1, objects:2, handles:2, threads:0},
                    event:{processes:1, objects:26, handles:26, threads:0, handleLimits:1},
                    thread:{processes:1, objects:1, handles:0, threads:2}
                }[item.name];
                for (const [field, count] of Object.entries(expected)) {
                    assert.equal(delta[field], count, item.name + ': measured ' + field);
                }
            }
            assert.deepEqual(await resources(), baseline, item.name + ': all execution resources released');
        });
    }
}
module.exports = {run};
