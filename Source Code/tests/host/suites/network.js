const assert = require('node:assert/strict');

async function run(api) {
    const {check, command, guestLog, until, processes, resources, value} = api;
    await check('loopback network runs in Ring 3 and releases its handles', async () => {
        const before = await resources();
        const faults = await value('process_faults');
        const offset = guestLog().length;
        await command('nettest'); // The dedicated launcher grants CAP_NETWORK.
        await until(() => guestLog().slice(offset).includes('NETWORK USER TEST PASS'), 'loopback network', 20000);
        await until(async () => !(await processes()).some(p => p.name === 'nettest.snx'), 'network process exit');
        assert.equal(await value('process_faults'), faults);
        assert.deepEqual(await resources(), before);
    });
}
module.exports = {run};
