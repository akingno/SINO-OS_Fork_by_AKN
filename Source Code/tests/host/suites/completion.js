const assert = require('node:assert/strict');

async function run(api) {
    const {check, resources, guestLog, value, launch, until, gone, processes, command} = api;
    let baseline = await resources();
    await check('durable results, rights, exhaustion and launch rollback', async () => {
        const offset = guestLog().length, faults = await value('process_faults');
        const pid = await launch('comptest', 'suite');
        await until(() => guestLog().slice(offset).includes('COMPLETION SUITE PASS'), 'completion suite', 30000);
        await gone(pid);
        assert.equal(await value('process_faults'), faults + 3);
        assert.deepEqual(await resources(), baseline);
    });
    await check('external termination remains distinct from cancellation', async () => {
        const offset = guestLog().length;
        const faults = await value('process_faults');
        const pid = await launch('comptest', 'external');
        await until(() => guestLog().slice(offset).includes('COMPLETION KILL TARGET READY'), 'kill target');
        const targets = (await processes()).filter(p => p.name === 'comptest.snx' && p.pid !== pid);
        assert.equal(targets.length, 1);
        await command('kill ' + targets[0].pid);
        await until(() => guestLog().slice(offset).includes('COMPLETION KILLED RESULT PASS'), 'typed killed result');
        await gone(pid);
        assert.equal(await value('process_faults'), faults);
        assert.deepEqual(await resources(), baseline);
    });
    for (const mode of ['orphan', 'abandon', 'observer-fault']) {
        await check('observer lifecycle: ' + mode, async () => {
            const offset = guestLog().length, faults = await value('process_faults');
            const pid = await launch('comptest', mode);
            await until(() => guestLog().slice(offset).includes('COMPLETION ORPHAN CHILD PASS'), mode);
            await gone(pid);
            await until(async () => (await processes()).length === 0, 'orphan reaped');
            assert.equal(await value('process_faults'), faults + (mode === 'observer-fault' ? 1 : 0));
            assert.deepEqual(await resources(), baseline);
        });
    }
}
module.exports = {run};
