/* Composition entry point. Functional cases live in leaf suites. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {spawn} = require('node:child_process');
const host = require('../../tools/host-tools');
const {sources} = require('../../tools/dev-run');
const catalog = require('../support/regression-catalog');
const {createPlan, assertCaseIds} = require('../support/regression-plan');
const {runVM} = require('../support/regression-vm');
const sourceRoot = path.resolve(__dirname, '../..');
const repoRoot = path.dirname(sourceRoot);
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const readJSON = file => JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
const writeJSON = (file, value) => fs.writeFileSync(file, JSON.stringify(value, null, 2) + '\n');

async function command(executable, args, output, env = {}) {
    const fd = fs.openSync(output, 'w');
    let child;
    try {
        child = spawn(executable, args, {cwd:sourceRoot, windowsHide:true,
            env:{...process.env, ...env}, stdio:['ignore', fd, fd]});
    } finally { fs.closeSync(fd); }
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => child.kill(), 120000);
        child.once('error', error => {clearTimeout(timer); reject(error);});
        child.once('exit', (code, signal) => {
            clearTimeout(timer);
            if (code === 0) resolve();
            else reject(Error('Command failed (' + (code ?? signal) + '): ' + output));
        });
    });
}

function plan(selection, full = false) {
    return createPlan(selection, {full}).names;
}
function printPlan(selection, preview = false, full = false) {
    const result = createPlan(selection, {full:preview || full});
    console.log('Plan: ' + result.names.length + ' groups; ' + result.count + ' unique cases');
    for (const name of result.names) {
        console.log('  ' + name + ': ' + catalog[name].checks);
        if (preview) for (const item of catalog[name].cases) console.log('    ' + item.id + '  ' + item.name);
    }
    return result;
}
async function run(directory, suite = 'core', {full = false} = {}) {
    const relative = path.relative(path.join(repoRoot, 'Build Output/dev/runs'), directory);
    assert(relative && !relative.startsWith('..') && !path.isAbsolute(relative) &&
        /^regression-[^/\\]+$/.test(relative), 'Use the repository regression.ps1 entry.');
    const executionPlan = createPlan(suite, {full});
    const names = executionPlan.names;
    const reportFile = path.join(directory, 'regression-report.json');
    const report = {version:4, suite, plannedCases:executionPlan.count, full, result:'FAIL', startedAt:new Date().toISOString(),
        scope:'headless functional regression',
        suites:names.map(name => ({name, result:'NOT-RUN'})), testHashes:{}};
    writeJSON(reportFile, report);
    let build;
    const seeds = new Map();
    try {
        build = readJSON(path.join(directory, 'build.json'));
        assert.equal(sources().sha256, build.sourceSha256, 'source identity before tests');
        assert.equal(hash(build.iso), build.isoSha256, 'ISO identity before tests');
        const symbols = path.join(directory, 'kernel-normal.exe');
        const extracted = path.join(directory, 'kernel-symbol-check.bin');
        await command(host.tool('objcopy'), ['-O', 'binary', symbols, extracted], path.join(directory, 'symbol-check.log'));
        assert.equal(hash(extracted), build.kernelSha256, 'symbol snapshot matches built kernel');
        build.symbolsSha256 = hash(symbols);
        report.build = build;
        for (const file of ['regression.ps1', 'test.ps1', 'run.ps1', 'Source Code/tests/host/test-core.js'])
            report.testHashes[file] = hash(path.join(repoRoot, file));
        for (const item of report.suites) {
            item.result = 'RUNNING';
            item.startedAt = new Date().toISOString();
            writeJSON(reportFile, report);
            console.log('RUN ' + item.name);
            try {
                const definition = catalog[item.name];
                const result = definition.host ? await definition.host({directory, command, readJSON}) :
                    await runVM({directory, build, name:item.name, definition, seeds});
                Object.assign(item, result);
                assertCaseIds(item.caseIds || [], definition.cases);
                assert(item.checks > 0, item.name + ': no checks executed');
                item.result = 'PASS';
                console.log('PASS ' + item.name + ': ' + item.checks + ' checks');
            } catch (error) { item.result = 'FAIL'; item.error = error.message; throw error; }
            finally { item.finishedAt = new Date().toISOString(); writeJSON(reportFile, report); }
        }
        report.result = 'PASS';
    } catch (error) { report.error = error.stack || String(error); console.error(report.error); }
    finally {
        try {
            if (build && (sources().sha256 !== build.sourceSha256 || hash(build.iso) !== build.isoSha256))
                throw Error('Source or ISO changed during verification.');
            for (const [name, expected] of Object.entries(report.testHashes))
                assert.equal(hash(path.join(repoRoot, name)), expected, 'test changed during run: ' + name);
        } catch (error) { report.result = 'FAIL'; report.identityError = error.message; }
        report.privateSeeds = Object.fromEntries(seeds);
        report.finishedAt = new Date().toISOString();
        report.summary = {passed:report.suites.filter(item => item.result === 'PASS').length,
            failed:report.suites.filter(item => item.result === 'FAIL').length,
            notRun:report.suites.filter(item => item.result === 'NOT-RUN').length,
            checks:report.suites.reduce((sum, item) => sum + (item.result === 'PASS' ? item.checks : 0), 0)};
        if (report.summary.passed !== names.length) report.result = 'FAIL';
        writeJSON(reportFile, report);
        console.log(report.result + ': ' + suite + ' ' + report.summary.passed + '/' + names.length +
            ' suites; ' + report.summary.checks + ' checks');
        console.log('Report: ' + reportFile);
    }
    return report.result === 'PASS' ? 0 : 1;
}

function cli() {
    const args = process.argv.slice(2), full = args[2] === '--full';
    if (args.length !== 2 && !(args.length === 3 && full)) { console.error('Use regression.ps1 from the repository root.'); process.exitCode = 1; return; }
    if (args[0] === '--validate' || args[0] === '--list') {
        try { printPlan(args[1], args[0] === '--list', full); }
        catch (error) { console.error(error.message); process.exitCode = 1; }
        return;
    }
    run(path.resolve(args[0]), args[1], {full}).then(code => {process.exitCode = code;})
        .catch(error => {console.error(error); process.exitCode = 1;});
}
if (require.main === module) cli();
module.exports = {cli, run, plan, printPlan};
