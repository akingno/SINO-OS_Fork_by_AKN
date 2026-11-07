const assert = require('node:assert/strict');

async function run(api) {
    const {check, resources, command, native, guestLog, value, until, gone} = api;
    const initial = await resources();
    await command('echo policy private > /home/user/contracts-persist.bin');
    const baseline = await resources();
    try {
    await check('legacy versioned queries preserve compatibility', async () => {
        assert.match(await command('help'), /run -s file.snx/);
        assert.match(await command('run -s', 1), /run -s native.snx/);
        await native('poltest', 'legacy', 'POLICY LEGACY PASS');
        assert.deepEqual(await resources(), baseline);
    });
    await check('strict policy denies every ambient entry and downgrade', async () => {
        await command('cp /boot/poltest.snx /home/user/poltest.snx');
        const offset = guestLog().length;
        const faults = await value('process_faults');
        const exits = await value('process_exits');
        const output = await command('run -s /BOOT/../boot/POLTEST.SNX strict');
        const pid = Number(output.match(/^Started user process (\d+)\n$/)?.[1]);
        assert(pid > 0);
        await until(() => guestLog().slice(offset).includes('POLICY STRICT PASS'), 'strict policy');
        await gone(pid);
        assert.equal(await value('process_faults'), faults);
        assert.equal(await value('process_exits'), exits + 1);
        const copyOffset = guestLog().length;
        const copied = await command('run -s /home/user/poltest.snx strict');
        const copyPid = Number(copied.match(/^Started user process (\d+)\n$/)?.[1]);
        assert(copyPid > 0);
        await until(() => guestLog().slice(copyOffset).includes('POLICY STRICT PASS'), 'strict writable copy');
        await gone(copyPid);
        assert.equal(await value('process_faults'), faults);
        assert.equal(await value('process_exits'), exits + 2);
        await native('poltest', 'legacy', 'POLICY LEGACY PASS');
        await command('rm /home/user/poltest.snx');
        assert.deepEqual(await resources(), baseline);
        await command('cat /home/user/policy-escape', 1);
    });
    } finally { await command('rm /home/user/contracts-persist.bin'); }
    assert.deepEqual(await resources(), initial, 'policy fixtures reclaimed');
}
module.exports = {run};
