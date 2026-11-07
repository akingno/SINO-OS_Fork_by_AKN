const assert = require('node:assert/strict');
async function run({check, native, resources}) {
    let baseline;
    await check('prepare the stress probe resource baseline', async () => {
        const before = await resources();
        await native('probe', '/contracts', 'CONTRACT PROBE PASS checks=');
        baseline = await resources();
        assert.deepEqual(baseline, {...before, freePages:before.freePages - 1});
    });
    for (let i = 1; i <= 8; i++) {
        await check(`normal exit resource recovery ${i}/8`, async () => {
            await native('probe', '/contracts', 'CONTRACT PROBE PASS checks=');
            assert.deepEqual(await resources(), baseline);
        });
    }
}
module.exports = {run};
