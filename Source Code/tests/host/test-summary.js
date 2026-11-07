/* Read saved evidence without dumping successful commands or raw guest logs. */
const fs = require('node:fs');
const path = require('node:path');
const {select} = require('../support/regression-selection');
const compact = value => String(value || '').replace(/\s+/g, ' ').slice(0, 360);
function analyze(report) {
    const errors = [], suites = Array.isArray(report.suites) ? report.suites : [];
    let names = [];
    try { names = select(report.suite); }
    catch (error) { errors.push(error.message); }
    const summary = {passed:0, failed:0, notRun:0, checks:0};
    const seen = new Set();
    if (report.version !== 3) errors.push('Unsupported report version');
    if (!['PASS', 'FAIL'].includes(report.result)) errors.push('Missing final result');
    for (const suite of suites) {
        if (!suite || !names.includes(suite.name) || seen.has(suite.name)) {
            errors.push('Unknown, duplicate or malformed suite'); continue;
        }
        seen.add(suite.name);
        if (suite.result === 'PASS') {
            summary.passed++;
            if (!Number.isSafeInteger(suite.checks) || suite.checks <= 0)
                errors.push(suite.name + ': missing positive check count');
            else summary.checks += suite.checks;
            if (suite.error) errors.push(suite.name + ': PASS contains an error');
        } else if (suite.result === 'FAIL') summary.failed++;
        else {
            summary.notRun++;
            if (!['NOT-RUN', 'RUNNING'].includes(suite.result)) errors.push(suite.name + ': invalid state');
        }
    }
    summary.notRun += names.filter(name => !seen.has(name)).length;
    if (report.summary) {
        for (const key of Object.keys(summary))
            if (report.summary[key] !== summary[key]) errors.push('Summary mismatch: ' + key);
    } else errors.push('Missing final summary');
    if (report.error) errors.push(compact(report.error));
    if (report.identityError) errors.push('Identity: ' + compact(report.identityError));
    if (report.result === 'PASS') {
        if (!report.finishedAt) errors.push('Missing completion time');
        for (const key of ['sourceSha256', 'isoSha256', 'kernelSha256'])
            if (!/^[a-f0-9]{64}$/i.test(report.build?.[key] || '')) errors.push('Missing build identity: ' + key);
        if (names.includes('runner-rejects-failure') &&
            suites.find(s => s?.name === 'runner-rejects-failure')?.expectedFailure !== true)
            errors.push('Expected-failure rejection was not confirmed');
    }
    return {names, suites, summary, errors, passed:report.result === 'PASS' &&
        summary.passed === names.length && !summary.failed && !summary.notRun && !errors.length};
}
function readJSON(file) { return JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, '')); }
function run(directory) {
    const file = path.join(path.resolve(directory), 'regression-report.json');
    const report = readJSON(file), result = analyze(report);
    const names = result.names;
    const s = result.summary;
    console.log(`${result.passed ? 'PASS' : 'FAIL'} ${path.basename(path.dirname(file))}: ` +
        `${s.passed}/${names.length} groups; ${s.checks} checks; failed=${s.failed}, not-run=${s.notRun}`);
    for (const name of names) {
        const suite = result.suites.find(s => s?.name === name);
        console.log(`  ${suite?.result || 'MISSING'} ${name}: ${suite?.checks || 0}` +
            (suite?.expectedFailure ? ' (expected rejection verified)' : ''));
        if (suite?.guestAssertions) console.log(`    guest assertions=${suite.guestAssertions}`);
        if (suite?.error) console.log('    ' + compact(suite.error));
    }
    for (const error of result.errors.slice(0, 6)) console.log('  ERROR ' + compact(error));
    /* Only failed groups trigger child-report inspection; bound displayed cases. */
    const children = Object.fromEntries(names.map(name => [name, name + '/shell-report.json']));
    children.infrastructure = 'infrastructure/src32-refactor-report.json';
    for (const suite of result.suites.filter(s => s?.result === 'FAIL')) {
        const child = children[suite.name];
        if (!child) continue;
        const target = path.join(path.dirname(file), child);
        if (!fs.existsSync(target)) continue;
        try {
            const detail = readJSON(target);
            if (detail.error) console.log('  DETAIL ' + compact(detail.error));
            const cases = detail.cases || detail.checks || [];
            for (const item of cases.filter(c => c.result !== 'PASS').slice(0, 3))
                console.log('  CASE ' + compact(item.name || item.id) + ': ' + compact(item.error || item.detail || item.result));
        } catch (error) { console.log('  DETAIL unavailable: ' + compact(error.message)); }
    }
    console.log('Report: ' + file);
    console.log('Saved report summary; does not rerun tests or recheck current source/ISO identity.');
    return result.passed ? 0 : 1;
}
function cli() {
    try {
        if (process.argv.length !== 3) throw Error('Usage: test-summary.js <exact run directory>');
        process.exitCode = run(process.argv[2]);
    } catch (error) { console.error('FAIL summary: ' + compact(error.message)); process.exitCode = 1; }
}
if (require.main === module) cli();
module.exports = {cli, analyze, run};
