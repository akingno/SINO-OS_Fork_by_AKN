const assert = require('node:assert/strict');
const cases = require('../../support/thread-cases.json');
async function run(api) {
    const {check, command, value, resources, guestLog, until, gone} = api;
    async function probe(mode, app = 'probe', outcome = 'exit') {
        const baseline = await resources();
        const faults = await value('process_faults');
        const offset = guestLog().length;
        // 严格进程尚无通用子进程委托；这两项用兼容父进程创建严格子进程。
        const strict = mode !== 'compete' && mode !== 'last';
        const output = await command('run ' + (strict ? '-s ' : '') + '/boot/' + app + '.snx /thread-' + mode);
        const pid = Number(output.match(/^Started user process (\d+)\n$/)?.[1]);
        assert(pid, output);
        if (outcome === 'cancel') {
            await until(() => guestLog().slice(offset).includes('THREAD CANCEL PARKED'), 'join park');
            await command('kill ' + pid);
        }
        await gone(pid);
        const log = guestLog().slice(offset);
        assert.equal(await value('process_faults'), faults + (outcome === 'fault' ? 1 : 0));
        if (outcome === 'exit') { assert(log.includes('THREAD PROBE PASS /thread-' + mode), log); }
        if (outcome === 'fault') { assert.match(log, /THREAD EXPECT FAULT[\s\S]*USER fault contained.*vector=0x0000000e/i); }
        assert.deepEqual(await resources(), baseline, 'thread stacks, records and wait ownership reclaimed');
    }
    for (const item of cases) {
        await check(item.name, async () => {
            if (item.name === 'cancel-fault') {
                await probe('cancel', 'probe', 'cancel');
                await probe('fault', 'probe', 'fault');
                await probe('record');
            } else {
                await probe(item.name);
                if (item.name === 'return') { await probe(item.name, 'threadv2'); }
            }
        });
    }
}
module.exports = {run};
