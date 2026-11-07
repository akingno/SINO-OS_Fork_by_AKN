/* Pipe waiting contracts on a recorded build and private disk. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {run} = require('../../tools/shell-test');
const {RunSession} = require('../../tools/run-channel');

const program = '/boot/pipetest.snx';
const modes = ['read-wake', 'write-wake', 'read-eof', 'write-closed', 'read-timeout',
    'contended-read', 'local-close', 'unmap', 'wire'];
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const readJSON = file => JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));

async function verify({channel, report, guestLog}) {
    let id = 0;
    async function command(text) {
        const reply = await channel.request(++id, text, 5000);
        assert.equal(reply.status, 0, text + ': ' + reply.output);
        assert.equal(reply.overflow, false);
        return reply.output.toString();
    }
    async function memory() {
        const match = (await command('mem')).match(/\/ (\d+) pages/);
        assert(match, 'missing free page count');
        return Number(match[1]);
    }
    const baseline = await memory();
    async function check(name, action) {
        const item = {name, result:'FAIL'};
        report.cases.push(item);
        const offset = guestLog().length;
        try {
            await action();
            assert.equal(await memory(), baseline, 'case leaked guest pages');
            item.result = 'PASS';
        } catch (error) {
            const failure = guestLog().slice(offset).match(/PIPE SELFTEST FAIL:[^\n]*/);
            item.error = failure?.[0] || error.message;
            throw Error(item.error);
        }
    }
    async function compat(mode, marker) {
        const offset = guestLog().length;
        const pid = (await command(`run ${program} ${mode}`)).match(/Started user process (\d+)/)?.[1];
        assert(pid, 'compatibility probe launch failed');
        const deadline = Date.now() + 20000;
        while (new RegExp('^' + pid + '\\s', 'm').test(await command('ps'))) {
            assert(Date.now() < deadline, 'probe did not exit: ' + mode);
            await sleep(25);
        }
        assert.match(guestLog().slice(offset), marker);
    }
    await check('legacy nonblocking pipe suite', () => compat('suite', /PIPE SUITE PASS checks=/));
    const session = new RunSession(channel);
    await session.hello();
    for (const mode of modes) {
        await check(mode, async () => {
            const result = await session.run(program, ['b:' + mode]);
            assert.equal(result.reason, 1, 'expected normal process exit');
            assert.equal(result.code, 0, 'pipe probe exit code');
        });
    }
    await check('blocked process timeout releases pipe', () =>
        compat('block-release', /PIPE BLOCK CHILD ENTER[\s\S]*PIPE BLOCK RELEASE PASS/));
}

async function main(source) {
    const build = readJSON(path.join(source, 'build.json'));
    const directory = fs.mkdtempSync(path.join(source, 'pipe-waits-'));
    const iso = path.join(directory, 'SINO.iso');
    fs.copyFileSync(build.iso, iso, fs.constants.COPYFILE_EXCL);
    fs.writeFileSync(path.join(directory, 'build.json'), JSON.stringify({...build, iso}));
    fs.writeFileSync(path.join(directory, 'shell-job.json'), JSON.stringify({timeoutMs:5000}));
    const code = await run(directory, {execute:verify});
    const report = readJSON(path.join(directory, 'shell-report.json'));
    const passed = report.cases.filter(item => item.result === 'PASS').length;
    const expected = modes.length + 2;
    const success = code === 0 && passed === expected && report.vmStopped;
    console.log(`${success ? 'PASS' : 'FAIL'} pipe waits ${passed}/${expected} cases`);
    for (const item of report.cases.filter(item => item.result !== 'PASS'))
        console.error(item.name + ': ' + item.error);
    return success ? 0 : 1;
}

if (process.argv.length !== 3) {
    console.error('Usage: node test-pipe-waits.js <ShellBridge build directory>');
    process.exitCode = 2;
} else {
    main(path.resolve(process.argv[2])).then(code => {process.exitCode = code;})
        .catch(error => {console.error(error.message); process.exitCode = 1;});
}
