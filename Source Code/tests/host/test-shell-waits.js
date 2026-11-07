const assert = require('node:assert/strict');
const {createHash} = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const {execFileSync} = require('node:child_process');
const host = require('../../tools/host-tools');
const {runSuite} = require('../support/wait-tests');

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function observeShellWait({directory, qmp, report, build}, log) {
    const snapshot = path.join(directory, 'kernel-normal.exe');
    const flat = path.join(directory, 'kernel-symbol-check.bin');
    fs.copyFileSync(path.join(directory, '..', 'kernel-normal.exe'), snapshot, fs.constants.COPYFILE_EXCL);
    execFileSync(host.tool('objcopy'), ['-O', 'binary', snapshot, flat], {windowsHide:true});
    const hash = createHash('sha256').update(fs.readFileSync(flat)).digest('hex');
    assert(hash === build.kernelSha256, 'kernel symbol snapshot does not match the recorded kernel');
    const symbols = execFileSync(host.tool('nm'), ['-n', snapshot],
        {encoding:'utf8', windowsHide:true});
    const addresses = new Map();
    for (const line of symbols.split('\n')) {
        const match = line.match(/^([0-9a-f]+) [A-Za-z] _(ticks|process_switches)\s*$/);
        if (match) addresses.set(match[2], match[1]);
    }
    let seq = 0;
    async function value(name) {
        const address = addresses.get(name);
        assert(address, 'missing kernel symbol ' + name);
        const file = 'wait-counter-' + ++seq + '.bin';
        await qmp.hmp(`pmemsave 0x${address} 4 "${file}"`);
        return fs.readFileSync(path.join(directory, file)).readUInt32LE();
    }
    const deadline = Date.now() + 8000;
    while (!log().includes('SHELL WAIT HOLD')) {
        assert(Date.now() < deadline, 'shell did not reach the withheld broker result');
        await sleep(20);
    }
    await sleep(60);
    const start = await value('ticks');
    const before = await value('process_switches');
    let ticks = 0;
    while (ticks < 15) {
        assert(Date.now() < deadline && !log().includes('SHELL WAIT RELEASE'),
            'broker released before the quiet interval could be measured');
        await sleep(30);
        ticks = ((await value('ticks')) - start) >>> 0;
    }
    const dispatches = ((await value('process_switches')) - before) >>> 0;
    assert(!log().includes('SHELL WAIT RELEASE'), 'measurement crossed the broker release');
    report.shellWait = {ticks, dispatches};
    assert.equal(dispatches, 0, 'user dispatches while Shell awaited broker result');
}

runSuite('shell-waits', [
    ['headless Shell result wait', 'brokerqa', 'shell-wait', 'SHELL PASS', {observe:observeShellWait}],
    ['headless Shell broker failure', 'brokerqa', 'shell-error', 'SHELL PASS'],
    ['SDK pipe waits and write-all deadline', 'pipetest', 'suite', 'PIPE SUITE PASS checks='],
    ['legacy terminal and output retry', 'termtest', 'suite', 'TERMINAL SUITE PASS'],
    ['SDK terminal read wait', 'termtest', 'b:sdk-read', 'TERMINAL BLOCK PASS'],
    ['terminal hangup', 'termtest', 'b:hangup', 'TERMINAL BLOCK PASS'],
    ['completion wake', 'comptest', 'w:multi-cancel', 'COMPLETION BLOCK PASS'],
    ['child run timeout', 'comptest', 'w:run-timeout', 'COMPLETION BLOCK PASS'],
    ['result wait timeout', 'comptest', 'w:timeout', 'COMPLETION BLOCK PASS'],
    ['anonymous cross-page file and pipe I/O', 'fstest', 'anon-parent', 'ANON PARENT PASS'],
    ['thread slot reuse', 'poltest', 'paging-a', 'PAGING STRICT A PASS', {strict:true}],
    ['Shell broker wait interrupted by hangup', 'brokerqa', 'shell-hangup', 'SHELL WAIT HANGUP PASS'],
    ['terminal retry rejects unmapped output', 'termtest', 'b:unmap', 'TERMINAL BLOCK PASS']
], ['contended-read', 'write-wake'], [
    ['Shell pipeline', 'echo hello | cat', 'hello\n'],
    ['Shell quoting', 'echo "hello world"', 'hello world\n'],
    ['Shell command sequence', 'echo first; echo second', 'first\nsecond\n']
]);
