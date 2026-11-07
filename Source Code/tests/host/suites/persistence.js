const assert = require('node:assert/strict');
async function write({check, native, command}) {
        await check('headless probe commits exact binary bytes', () =>
            native('fstest', 'memory-save', 'MEMORY SAVE PASS'));
        await check('headless command channel commits text bytes', async () => {
            assert.equal(await command('echo durable marker > /home/user/persistence-marker.txt'), '');
            assert.match(await command('sync'), /Home data saved/);
        });
}
async function read({check, native, command}) {
        await check('cold boot recovers exact binary bytes', () =>
            native('fstest', 'memory-read', 'MEMORY READ PASS'));
        await check('cold boot recovers exact text bytes', async () => {
            assert.equal(await command('cat /home/user/persistence-marker.txt'), 'durable marker\n');
        });
}
function snapshot(records) {
    assert(records.get('/home/user/memory-persist.bin')?.data.equals(Buffer.from([0,127,255,10,71,0,51])));
    assert.equal(records.get('/home/user/persistence-marker.txt')?.data.toString(), 'durable marker\n');
}
module.exports = {write, read, snapshot};
