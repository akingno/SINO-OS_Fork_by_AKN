const assert = require('node:assert/strict');

async function run(api) {
    const {check, resources, guestLog, value, launch, until, gone, processes} = api;
    let baseline = await resources();
    await check('bounded pipes, concurrent streaming, close and deadline recovery', async item => {
        const offset = guestLog().length, faults = await value('process_faults');
        const pid = await launch('pipetest', 'suite');
        await until(() => guestLog().slice(offset).includes('PIPE SUITE PASS checks='), 'pipe suite', 30000);
        await gone(pid);
        const count = Number(guestLog().slice(offset).match(/PIPE SUITE PASS checks=(\d+)/)?.[1]);
        assert(count >= 100, 'incomplete pipe assertions');
        item.guestAssertions = count;
        assert.equal(await value('process_faults'), faults + 1);
        assert.equal((await processes()).length, 0);
        assert.deepEqual(await resources(), baseline);
    });
}
module.exports = {run};
