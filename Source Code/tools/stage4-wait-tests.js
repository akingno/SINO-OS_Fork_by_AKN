/* Focused wait probes on a recorded ShellBridge build and a private disk. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {run} = require('./shell-test');
const {RunSession} = require('./run-channel');

const readJSON = file => JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));

async function verify({channel, report, guestLog}, cases, pipeCases) {
    let id = 0;
    async function command(text) {
        const reply = await channel.request(++id, text, 5000);
        assert.equal(reply.status, 0, text + ': ' + reply.output);
        assert.equal(reply.overflow, false);
        return reply.output.toString();
    }
    async function freePages() {
        const pages = (await command('mem')).match(/\/ (\d+) pages/);
        assert(pages, 'missing free page count');
        return Number(pages[1]);
    }
    const baseline = await freePages();
    async function check(name, action) {
        const item = {name, result:'FAIL'};
        report.cases.push(item);
        const offset = guestLog().length;
        try {
            await action(offset);
            assert.equal(await freePages(), baseline, 'case leaked guest pages');
            item.result = 'PASS';
        } catch (error) {
            const failure = guestLog().slice(offset).match(/(?:COMPLETION|CLI|PIPE|TERMINAL) SELFTEST FAIL:[^\n]*/);
            item.error = failure?.[0] || error.message;
            throw Error(item.error);
        }
    }
    for (const [name, app, args, marker] of cases) {
        await check(name, async offset => {
            const output = await command(`run /boot/${app}.snx ${args}`);
            const pid = output.match(/Started user process (\d+)/)?.[1];
            assert(pid, 'probe launch failed: ' + output);
            const alive = new RegExp('^' + pid + '\\s', 'm');
            const deadline = Date.now() + 15000;
            while (alive.test(await command('ps'))) {
                assert(Date.now() < deadline, 'probe did not exit');
                await new Promise(resolve => setTimeout(resolve, 25));
            }
            assert(guestLog().slice(offset).includes(marker), 'missing marker ' + marker);
        });
    }
    const session = new RunSession(channel);
    await session.hello();
    for (const mode of pipeCases) {
        await check('pipe ' + mode, async () => {
            const result = await session.run('/boot/pipetest.snx', ['b:' + mode]);
            assert.equal(result.reason, 1, 'expected normal process exit');
            assert.equal(result.code, 0, 'pipe probe exit code');
        });
    }
}

async function main(source, phase, cases, pipeCases) {
    const build = readJSON(path.join(source, 'build.json'));
    const directory = fs.mkdtempSync(path.join(source, 'phase-' + phase.toLowerCase() + '-'));
    const iso = path.join(directory, 'SINO.iso');
    fs.copyFileSync(build.iso, iso, fs.constants.COPYFILE_EXCL);
    fs.writeFileSync(path.join(directory, 'build.json'), JSON.stringify({...build, iso}));
    fs.writeFileSync(path.join(directory, 'shell-job.json'), JSON.stringify({timeoutMs:5000}));
    const code = await run(directory, {execute:context => verify(context, cases, pipeCases)});
    const report = readJSON(path.join(directory, 'shell-report.json'));
    const passed = report.cases.filter(item => item.result === 'PASS').length;
    const expected = cases.length + pipeCases.length;
    const success = code === 0 && passed === expected && report.vmStopped;
    console.log(`${success ? 'PASS' : 'FAIL'} Phase ${phase} ${passed}/${expected} cases`);
    for (const item of report.cases.filter(item => item.result !== 'PASS'))
        console.error(item.name + ': ' + item.error);
    return success ? 0 : 1;
}

function runPhase(phase, cases, pipeCases) {
    if (process.argv.length !== 3) {
        console.error('Usage: node ' + path.basename(process.argv[1]) + ' <ShellBridge build directory>');
        process.exitCode = 2;
        return;
    }
    main(path.resolve(process.argv[2]), phase, cases, pipeCases)
        .then(code => {process.exitCode = code;})
        .catch(error => {console.error(error.message); process.exitCode = 1;});
}
module.exports = {runPhase};
