const assert = require('node:assert/strict');
async function run({check, resources, native}) {
    const baseline = await resources();
    await check('file manager window selftest and resource recovery', async () => {
        await native('files', '/selftest', 'FILES SELFTEST PASS');
        assert.deepEqual(await resources(), baseline);
    });
}
module.exports = {run};
