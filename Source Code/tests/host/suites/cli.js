const assert = require('node:assert/strict');

async function run(api) {
    const {check, resources, guestLog, value, launch, until, gone} = api;
    let baseline = await resources();
    await check('bounded CLI inputs, streams, capture, quota and rollback', async () => {
        const offset = guestLog().length, faults = await value('process_faults');
        const pid = await launch('clitest', 'unit');
        await until(() => guestLog().slice(offset).includes('CLI SUITE PASS'), 'CLI suite', 30000);
        await gone(pid);
        assert.equal(await value('process_faults'), faults + 1);
        assert.deepEqual(await resources(), baseline);
    });
}
module.exports = {run};
