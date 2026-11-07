const assert = require('node:assert/strict');
async function read({check, command}) {
    await check('cold boot preserves GUI Shell committed bytes', async () => {
        assert.equal(await command('cat /home/user/g5b-durable.txt'), 'g5b durable\n');
    });
}
function snapshot(records) {
    assert.equal(records.get('/home/user/g5b-durable.txt')?.data.toString(), 'g5b durable\n');
}
module.exports = {read, snapshot};
