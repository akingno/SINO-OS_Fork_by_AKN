const assert = require('node:assert/strict');

async function run(api) {
    const {check, resources, guestLog, value, launch, until, gone, processes, report, command} = api;
    let baseline = await resources();
    await check('directory grants, range IO, private publication and durable save', async item => {
        const offset = guestLog().length, faults = await value('process_faults');
        const pid = await launch('fstest', 'suite');
        await until(() => guestLog().slice(offset).includes('FILESYS SUITE PASS checks='), 'file object suite', 30000);
        await gone(pid);
        const output = guestLog().slice(offset);
        const count = Number(output.match(/FILESYS SUITE PASS checks=(\d+)/)?.[1]);
        assert(count >= 45, 'incomplete file object assertions');
        item.guestAssertions = count;
        assert.equal(await value('process_faults'), faults + 1);
        assert.equal((await processes()).length, 0);
        const savedBaseline = {...baseline, freePages:baseline.freePages - 1};
        assert.deepEqual(await resources(), savedBaseline, 'only the new durable 257-byte file retains a page');
        baseline = savedBaseline;
        report.resourceBaseline = baseline;
        await command('cat /home/user/contracts-files/private', 1);
    });
}
module.exports = {run};
module.exports.snapshot = records => {
    const expected = Buffer.from(Array.from({length:257}, (_, i) => (i * 19 + 7) & 255));
    assert(records.get('/home/user/contracts-file-save.bin')?.data.equals(expected), 'CRC-valid files snapshot bytes');
};
