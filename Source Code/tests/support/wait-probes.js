const assert = require('node:assert/strict');
const {RunSession} = require('../../tools/run-channel');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function probe(api, app, args, marker, options = {}) {
    const offset = api.guestLog().length;
    const output = await api.command('run ' + (options.strict ? '-s ' : '') + '/boot/' + app + '.snx ' + args);
    const pid = Number(output.match(/Started user process (\d+)/)?.[1]);
    assert(pid, 'probe launch failed: ' + output);
    if (options.observe) await options.observe(api, () => api.guestLog().slice(offset));
    await api.gone(pid);
    assert.match(api.guestLog().slice(offset), marker);
}
async function quietShell(api, log) {
    const deadline = Date.now() + 8000;
    while (!log().includes('SHELL WAIT HOLD')) {
        assert(Date.now() < deadline, 'Shell did not withhold its broker result');
        await sleep(20);
    }
    await sleep(60);
    const start = await api.value('ticks'), before = await api.value('process_switches');
    let ticks = 0;
    while (ticks < 15) {
        assert(Date.now() < deadline && !log().includes('SHELL WAIT RELEASE'), 'broker released before measurement');
        await sleep(30); ticks = ((await api.value('ticks')) - start) >>> 0;
    }
    const dispatches = ((await api.value('process_switches')) - before) >>> 0;
    assert(!log().includes('SHELL WAIT RELEASE'), 'measurement crossed broker release');
    api.report.shellWait = {ticks, dispatches};
    assert.equal(dispatches, 0, 'user dispatches while Shell waited');
}
async function checkedProbe(api, name, app, args, marker, options) {
    const baseline = (await api.resources()).freePages;
    await api.check(name, async () => {
        await probe(api, app, args, marker, options);
        assert.equal((await api.resources()).freePages, baseline, 'probe leaked guest pages');
    });
}
async function pipeModes(api, modes) {
    const baseline = (await api.resources()).freePages;
    const session = new RunSession(api.channel);
    await session.hello();
    for (const mode of modes) await api.check(mode, async () => {
        const result = await session.run('/boot/pipetest.snx', ['b:' + mode]);
        assert.equal(result.reason, 1, 'normal pipe probe exit');
        assert.equal(result.code, 0, 'pipe probe exit code');
        assert.equal((await api.resources()).freePages, baseline, 'pipe probe leaked pages');
    });
}
module.exports = {checkedProbe, pipeModes, quietShell};
