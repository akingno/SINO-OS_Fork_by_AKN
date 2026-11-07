const assert = require('node:assert/strict');

async function run(api) {
    const {check, resources, guestLog, value, launch, until, gone, processes} = api;
    let baseline = await resources();
    await check('terminal isolation, foreground input and recovery', async () => {
        const offset = guestLog().length, faults = await value('process_faults');
        const pid = await launch('termtest', 'suite');
        await until(() => guestLog().slice(offset).includes('TERMINAL SUITE PASS'), 'terminal suite', 30000);
        await gone(pid);
        assert.equal(await value('process_faults'), faults + 1);
        assert.equal((await processes()).length, 0);
        assert.deepEqual(await resources(), baseline);
    });
}
module.exports = {run};
