const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawnSync} = require('node:child_process');
const {analyze} = require('./test-summary');
const {select} = require('../support/regression-selection');
const group = process.argv[2] === '--group' && process.argv.length === 4 ? process.argv[3] : 'all';
assert(['all','contracts','selections','io'].includes(group), 'Unknown report test group');
const fixture = (suite = 'core') => ({version:3, suite, result:'PASS', finishedAt:'2026-11-05T00:00:00Z',
    build:{sourceSha256:'a'.repeat(64), isoSha256:'b'.repeat(64), kernelSha256:'c'.repeat(64)},
    suites:select(suite).map(name => ({name, result:'PASS', checks:1,
        ...(name === 'runner-rejects-failure' ? {expectedFailure:true} : {})})),
    summary:{passed:select(suite).length, failed:0, notRun:0, checks:select(suite).length}});
let checks = 0;
function check(ok) { assert(ok); checks++; }
if (group === 'all' || group === 'contracts') {
check(analyze(fixture()).passed);
for (const alter of [
    r => {r.result = 'FAIL';}, r => {r.version = 2;}, r => {delete r.finishedAt;},
    r => {r.suites.pop();}, r => {r.suites[1] = r.suites[0];},
    r => {r.suites[0].result = 'RUNNING';}, r => {r.suites[0].result = 'NOT-RUN';},
    r => {r.suites[0].checks = 0;}, r => {r.suites[0].checks = '1';},
    r => {r.summary.checks++;}, r => {delete r.summary;}, r => {delete r.build;},
    r => {r.identityError = 'changed';}, r => {r.error = 'failed';},
    r => {r.suites[0].error = 'failed';}, r => {r.suites.find(item => item.name === 'runner-rejects-failure').expectedFailure = false;}
]) { const r = fixture(); alter(r); check(!analyze(r).passed); }
check(!analyze({version:1, result:'FAIL', stage:'build', suites:[], testsExecuted:false}).passed);
}
if (group === 'all' || group === 'selections') {
for (const suite of ['persistence', 'terminal-gui']) check(analyze(fixture(suite)).passed);
const wrongSelection = fixture('persistence'); wrongSelection.suite = 'terminal-gui';
check(!analyze(wrongSelection).passed);
const unknownSelection = fixture(); unknownSelection.suite = 'unknown';
check(!analyze(unknownSelection).passed);
}
if (group === 'all' || group === 'io') {
const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'sino-summary-'));
try {
    const file = path.join(folder, 'regression-report.json');
    const invoke = () => spawnSync(process.execPath, [path.join(__dirname, 'test-summary.js'), folder],
        {encoding:'utf8', windowsHide:true});
    fs.writeFileSync(file, '\uFEFF' + JSON.stringify(fixture()));
    let run = invoke(); check(run.status === 0 && run.stdout.includes(select('core').length + '/' + select('core').length + ' groups'));
    fs.writeFileSync(file, '{invalid'); run = invoke();
    check(run.status === 1 && run.stderr.includes('FAIL summary:'));
    fs.unlinkSync(file); run = invoke(); check(run.status === 1);
    const report = fixture(); report.result = 'FAIL';
    const failed = report.suites.find(item => item.name === 'files');
    failed.result = 'FAIL'; failed.checks = 0;
    const passed = report.suites.length - 1;
    report.summary = {passed, failed:1, notRun:0, checks:passed};
    fs.writeFileSync(file, JSON.stringify(report));
    fs.mkdirSync(path.join(folder, 'files'));
    fs.writeFileSync(path.join(folder, 'files/shell-report.json'), JSON.stringify({error:'wrong output',
        cases:[{name:'range read', result:'FAIL', error:'expected 3, got 2'}]}));
    run = invoke(); check(run.status === 1 && run.stdout.includes('CASE range read: expected 3, got 2'));
    fs.unlinkSync(path.join(folder, 'files/shell-report.json'));
    fs.rmdirSync(path.join(folder, 'files'));
    fs.unlinkSync(file);
} finally { fs.rmdirSync(folder); }
}
console.log(`SUMMARY TEST PASS: ${checks} checks`);
