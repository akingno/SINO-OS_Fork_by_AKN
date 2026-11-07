const assert = require('node:assert/strict');

async function run(api) {
    const {check, guestLog, resources, value, command, until, processes, gone} = api;
    let baseline = await resources();
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
}
module.exports = {run};
