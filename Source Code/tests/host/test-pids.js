// Explicit independent specialty; shared VM/transport only, never invokes another business suite.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {execFileSync} = require('node:child_process');
const {sources} = require('../../tools/dev-run');
const {tool} = require('../../tools/host-tools');
const {runVM} = require('../support/regression-vm');
const cases = require('../support/pid-cases.json');
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');

async function main(args) {
    if (args.length === 1 && args[0] === '--list') {
        console.log(cases.map(item => item.id + '  ' + item.name).join('\n') + '\n8 cases');
        return;
    }
    assert.equal(args.length, 1, 'Usage: node tests/host/test-pids.js <ShellBridge build directory>|--list');
    const directory = path.resolve(args[0]);
    const build = JSON.parse(fs.readFileSync(path.join(directory, 'build.json'), 'utf8').replace(/^\uFEFF/, ''));
    assert.equal(sources().sha256, build.sourceSha256, 'source identity before PID tests');
    assert.equal(hash(build.iso), build.isoSha256, 'ISO identity');
    const symbols = path.join(directory, 'kernel-normal.exe');
    const name = 'pids-' + crypto.randomBytes(4).toString('hex');
    const binary = path.join(directory, name + '-kernel.bin');
    execFileSync(tool('objcopy'), ['-O', 'binary', symbols, binary], {windowsHide:true});
    assert.equal(hash(binary), build.kernelSha256, 'matching symbol snapshot');
    build.symbolsSha256 = hash(symbols);
    const result = await runVM({directory, build, name,
        definition:{run:require('./suites/pids').run, cases, checks:cases.length}, seeds:new Map()});
    assert.equal(sources().sha256, build.sourceSha256, 'source identity after PID tests');
    assert.equal(hash(build.iso), build.isoSha256, 'ISO unchanged');
    const report = path.join(directory, 'pids-report.json');
    fs.writeFileSync(report, JSON.stringify({result:'PASS', checks:result.checks, buildId:build.buildId,
        cases:cases.map(item => ({...item, result:'PASS'})), vmReport:result.report}, null, 2));
    console.log('PASS PID lifecycle: 8 cases\nReport: ' + report);
}
main(process.argv.slice(2)).catch(error => { console.error(error.stack); process.exitCode = 1; });
