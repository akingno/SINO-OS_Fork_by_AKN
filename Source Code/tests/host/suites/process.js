const assert = require('node:assert/strict');

async function run(api) {
    const {check, guestLog, processes, native, resources, report, value, launch, until, gone, command, memory, symbols} = api;
    async function scheduleStats() {
        const bytes = await memory(symbols.task_schedule_stats, 60);
        const fields = ['passes', 'dispatches', 'noDispatch', 'candidates', 'retriesBlocked',
            'cleanups', 'continued', 'idleHalts', 'idleWithWork', 'maxCandidates',
            'notifications', 'wakeVisited', 'finiteWakes', 'broadcastWakes', 'timerExpirations'];
        return Object.fromEntries(fields.map((name, index) => [name, bytes.readUInt32LE(index * 4)]));
    }
    await check('boot and kernel object contracts', async () => {
        assert.match(guestLog(), /PAGE allocator self-check passed/);
        assert.match(guestLog(), /SYNC IRQ guard self-check passed/);
        assert.match(guestLog(), /TASK kernel stack self-check passed \(32 KiB usable \+ 4 KiB guard\)/);
        assert.match(guestLog(), /TASK scheduler state self-check passed/);
        assert((await memory(symbols.idle_halt, 3)).equals(Buffer.from([0xfb, 0xf4, 0xc3])),
            'actual guest idle entry must be adjacent sti; hlt; ret');
        assert.match(guestLog(), /OBJECT handles rights\/stale\/release PASS/);
        assert.match(guestLog(), /CPL3 selectors 1B\/23, I\/O denied/);
        assert.equal((await processes()).length, 0);
    });
    let baseline;
    await check('headless Ring 3 ABI, permissions, threads, handles and binary VFS', async item => {
        const before = await resources();
        const output = await native('probe', '/contracts', 'CONTRACT PROBE PASS checks=');
        const count = Number(output.match(/CONTRACT PROBE PASS checks=(\d+)/)?.[1]);
        assert(count >= 70, 'incomplete headless assertions');
        item.guestAssertions = count;
        baseline = await resources();
        assert.deepEqual(baseline, {...before, freePages:before.freePages - 1},
            'only the 257-byte persistent binary retains one page');
        report.resourceBaseline = baseline;
    });
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
        const before = await scheduleStats();
        await native('probe', '/schedule', 'SCHEDULER PROBE PASS:');
        const after = await scheduleStats();
        assert(after.continued > before.continued, 'runnable work continues without HLT');
        assert(after.idleHalts > before.idleHalts, 'all sleeping/blocked permits idle');
        assert(after.retriesBlocked > before.retriesBlocked, 'stale notification reblocks after retry');
        assert(after.finiteWakes - before.finiteWakes >= 3, 'finite notification and remaining-byte handoff');
        assert.equal(after.idleWithWork, 0, 'no idle with runnable/due/reap work');
        assert(after.maxCandidates > 0 && after.maxCandidates <= 32, 'bounded ready snapshot');
        assert(after.cleanups >= after.dispatches, 'every dispatch restores kernel binding/CR3/IF');
        report.scheduler = {before, after};
        const pid = await launch('spin');
        try {
            const ticks = await value('ticks');
            await native('probe', '/contracts', 'CONTRACT PROBE PASS checks=');
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
}
module.exports = {run};
module.exports.snapshot = records => {
    const expected = Buffer.from(Array.from({length:257}, (_, i) => (i * 37 + 11) & 255));
    assert(records.get('/home/user/contracts-persist.bin')?.data.equals(expected), 'CRC-valid process snapshot bytes');
};
