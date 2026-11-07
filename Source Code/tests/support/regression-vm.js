const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const shell = require('../../tools/shell-test');
const {volume} = require('../../tools/test-volume');
const {createGuest} = require('./guest-context');
const {assertCaseIds} = require('./regression-plan');
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');

async function runVM({directory, build, name, definition, seeds}) {
    const folder = path.join(directory, name);
    fs.mkdirSync(folder);
    const iso = path.join(folder, 'SINO.iso');
    fs.copyFileSync(build.iso, iso, fs.constants.COPYFILE_EXCL);
    fs.writeFileSync(path.join(folder, 'build.json'), JSON.stringify({...build, iso}));
    fs.writeFileSync(path.join(folder, 'shell-job.json'), JSON.stringify({
        selfTest:true, timeoutMs:15000, suite:definition.shell, gui:!!definition.gui,
        casePlan:definition.cases
    }));
    const seed = definition.seedFrom && seeds.get(definition.seedFrom);
    if (definition.seedFrom) {
        assert(seed, 'missing explicit seed: ' + definition.seedFrom);
        assert.equal(hash(seed.path), seed.sha256, 'private seed unchanged before clone');
    }
    const execute = definition.run && (async context => {
        // Headless suites can inspect memory but cannot send input or read GUI scenes.
        const selected = definition.gui ? context : {...context, qmp:{hmp:command => {
            assert.match(command, /^pmemsave /, 'headless memory inspection only');
            return context.qmp.hmp(command);
        }}};
        let api = createGuest(selected);
        if (definition.gui) api = await require('./gui-context').createGui(api);
        await definition.run(api);
    });
    const code = await shell.run(folder, {execute, seedDisk:seed?.path});
    const reportFile = path.join(folder, 'shell-report.json');
    const result = JSON.parse(fs.readFileSync(reportFile, 'utf8'));
    assert.equal(result.vmStopped, true, name + ': test VM did not stop');
    assertCaseIds(result.cases, definition.cases);
    if (seed) assert.equal(hash(seed.path), seed.sha256, 'cold boot preserves original private disk');
    if (definition.expectedFailure) {
        assert.equal(code, 1, 'intentional guest failure must return nonzero');
        assert.equal(result.result, 'FAIL');
        assert.equal(result.stage, 'commands', 'failure must follow verified boot');
        assert.equal(result.cases.length, 1);
        assert.equal(result.cases[0].result, 'FAIL');
        assert.match(result.error, /USER PROBE FAIL: intentional core runner failure/);
        return {checks:1, caseIds:result.cases.map(item => item.id), expectedFailure:true, report:reportFile};
    }
    assert.equal(code, 0, name + ': runner failed; see ' + reportFile);
    assert.equal(result.result, 'PASS');
    assert.equal(result.cases.length, definition.checks, name + ': incomplete suite');
    assert(result.cases.every(item => item.result === 'PASS'));
    if (definition.snapshot) {
        const disk = path.join(folder, 'shell-data.img');
        definition.snapshot(volume(disk).current.records);
        seeds.set(name, {path:disk, sha256:hash(disk)});
    }
    return {checks:result.cases.length, caseIds:result.cases.map(item => item.id), report:reportFile,
        guestAssertions:result.cases.reduce((sum, item) => sum + (item.guestAssertions || 0), 0)};
}
module.exports = {runVM};
