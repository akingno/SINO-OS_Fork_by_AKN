const assert = require('node:assert/strict');
const cases = require('../../support/event-cases.json');

async function run(api) {
    const {check, command, value, memory, symbols, resources, guestLog, until, gone} = api;
    async function stats() {
        const b = await memory(symbols.task_schedule_stats, 60);
        return {dispatches:b.readUInt32LE(4), retries:b.readUInt32LE(16),
            idle:b.readUInt32LE(28), idleWithWork:b.readUInt32LE(32),
            visited:b.readUInt32LE(44), broadcasts:b.readUInt32LE(52)};
    }
    async function probe(mode, outcome = 'exit') {
        const offset = guestLog().length;
        const faults = await value('process_faults');
        const exits = await value('process_exits');
        const output = await command('run -s /boot/probe.snx /event-' + mode);
        const pid = Number(output.match(/^Started user process (\d+)\n$/)?.[1]);
        assert(pid, output);
        if (outcome === 'cancel') {
            await until(() => guestLog().slice(offset).includes('EVENT CANCEL PARKED'), 'event wait parked');
            await command('kill ' + pid);
        }
        await gone(pid);
        const log = guestLog().slice(offset);
        assert.equal(await value('process_faults'), faults + (outcome === 'fault' ? 1 : 0));
        assert.equal(await value('process_exits'), exits + (outcome === 'exit' ? 1 : 0));
        if (outcome === 'exit') { assert(log.includes('EVENT PROBE PASS /event-' + mode)); }
        if (outcome === 'fault') { assert.match(log, /EVENT EXPECT FAULT[\s\S]*USER fault contained.*vector=0x0000000e/i); }
    }
    for (const item of cases) {
        await check(item.name, async record => {
            const baseline = await resources();
            const before = await stats();
            if (item.name === 'cancel-fault') {
                await probe('cancel', 'cancel');
                assert.deepEqual(await resources(), baseline, 'cancel removes all event/timer ownership');
                await probe('fault', 'fault');
                assert.deepEqual(await resources(), baseline, 'fault removes all event/timer ownership');
                await probe('preset'); // 再次分配对象，旧等待者不能影响复用槽位。
            } else {
                await probe(item.name);
            }
            const after = await stats();
            assert.equal(after.idleWithWork, 0);
            assert.deepEqual(await resources(), baseline, 'event probe resources recovered');
            if (item.name === 'broadcast') {
                assert.equal(after.broadcasts - before.broadcasts, 2, 'exactly two parked waiters notified once');
                assert.equal(after.visited - before.visited, 2, 'notification visits only target waiters');
                assert(after.idle > before.idle, 'kernel idles while event waiters are parked');
                assert(after.dispatches - before.dispatches < 20, 'event wait does not dispatch on every tick');
            }
            if (item.name === 'reset') { assert(after.retries > before.retries, 'reset rechecks and reblocks'); }
            record.scheduler = {before, after};
        });
    }
}
module.exports = {run};
