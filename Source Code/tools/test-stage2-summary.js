const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawnSync} = require('node:child_process');
const {analyze} = require('./stage2-summary');
const names = ['infrastructure', 'rtc-driver', 'shell-transport', 'shell-commands',
    'native-contracts', 'managed-cli', 'cold-reboot', 'runner-rejects-failure'];
const fixture = () => ({version:1, result:'PASS', finishedAt:'2026-11-05T00:00:00Z',
    build:{sourceSha256:'a'.repeat(64), isoSha256:'b'.repeat(64), kernelSha256:'c'.repeat(64)},
    suites:names.map(name => ({name, result:'PASS', checks:1,
        ...(name === 'runner-rejects-failure' ? {expectedFailure:true} : {})})),
    summary:{passed:8, failed:0, notRun:0, checks:8}});
let checks = 0;
function check(ok) { assert(ok); checks++; }
check(analyze(fixture()).passed);
for (const alter of [
    r => {r.result = 'FAIL';}, r => {r.version = 2;}, r => {delete r.finishedAt;},
    r => {r.suites.pop();}, r => {r.suites[1] = r.suites[0];},
    r => {r.suites[0].result = 'RUNNING';}, r => {r.suites[0].result = 'NOT-RUN';},
    r => {r.suites[0].checks = 0;}, r => {r.suites[0].checks = '1';},
    r => {r.summary.checks++;}, r => {delete r.summary;}, r => {delete r.build;},
    r => {r.identityError = 'changed';}, r => {r.error = 'failed';},
    r => {r.suites[0].error = 'failed';}, r => {r.suites[7].expectedFailure = false;}
]) { const r = fixture(); alter(r); check(!analyze(r).passed); }
check(!analyze({version:1, result:'FAIL', stage:'build', suites:[], testsExecuted:false}).passed);
const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'sino-summary-'));
try {
    const file = path.join(folder, 'stage2-report.json');
    const invoke = () => spawnSync(process.execPath, [path.join(__dirname, 'stage2-summary.js'), folder],
        {encoding:'utf8', windowsHide:true});
    fs.writeFileSync(file, '\uFEFF' + JSON.stringify(fixture()));
    let run = invoke(); check(run.status === 0 && run.stdout.includes('8/8 groups'));
    fs.writeFileSync(file, '{invalid'); run = invoke();
    check(run.status === 1 && run.stderr.includes('FAIL summary:'));
    fs.unlinkSync(file); run = invoke(); check(run.status === 1);
    const report = fixture(); report.result = 'FAIL'; report.suites[4].result = 'FAIL';
    report.suites[4].checks = 0; report.summary = {passed:7, failed:1, notRun:0, checks:7};
    fs.writeFileSync(file, JSON.stringify(report));
    fs.mkdirSync(path.join(folder, 'native'));
    fs.writeFileSync(path.join(folder, 'native/shell-report.json'), JSON.stringify({error:'wrong output',
        cases:[{name:'range read', result:'FAIL', error:'expected 3, got 2'}]}));
    run = invoke(); check(run.status === 1 && run.stdout.includes('CASE range read: expected 3, got 2'));
    fs.unlinkSync(path.join(folder, 'native/shell-report.json'));
    fs.rmdirSync(path.join(folder, 'native'));
    fs.unlinkSync(file);
} finally { fs.rmdirSync(folder); }
console.log(`SUMMARY TEST PASS: ${checks} checks`);
