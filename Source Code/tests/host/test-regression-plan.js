const assert = require('node:assert/strict');
const {createPlan, validateCatalog, assertCaseIds} = require('../support/regression-plan');
const {createCaseRecorder} = require('../support/case-recorder');
const {analyze} = require('./test-summary');
const catalog = require('../support/regression-catalog');
let passed = 0;
async function test(id, action) { await action(); passed++; console.log('PASS ' + id); }
(async () => {
    await test('plan.overlap', () => {
        assert.deepEqual(createPlan('files,files').names, createPlan('files').names);
        assert.equal(createPlan('files,policy,files').count, 4);
    });
    await test('plan.union', () => {
        const combined = createPlan('process,files,process');
        assert.equal(combined.count, createPlan('process').count + createPlan('files').count);
        assert.equal(new Set(combined.cases.map(item => item.id)).size, combined.count);
    });
    await test('plan.stress-isolation', () => {
        const core = createPlan('core', {full:true});
        assert(!core.names.includes('process-stress'));
        assert.equal(core.count, 261);
        assert.equal(createPlan('stress').count, 9);
    });
    await test('plan.total-budget', () => {
        assert.throws(() => createPlan('core'), /limit 50/);
        assert(createPlan('core', {full:true}).count > 50);
    });
    await test('plan.suite-budget', () => {
        assert.throws(() => createPlan('shell-transport'), /exceeds 20/);
        assert.equal(createPlan('shell-transport', {full:true}).count, 25);
    });
    await test('plan.unknown', () => {
        for (const selection of ['', 'missing', 'files,', '__proto__', 'terminal-gui',
            'files-gui', 'frontend-gui', 'shell-gui', 'file-tools-gui', 'shell-tools-gui',
            'workspace-gui', 'line-edit-gui', 'gui-persistence-write', 'gui-persistence-read'])
            assert.throws(() => createPlan(selection), /Unknown regression/);
    });
    await test('plan.seed-order', () => {
        const {names} = createPlan('files,process,persistence,files');
        for (const name of names) if (catalog[name].seedFrom)
            assert(names.indexOf(catalog[name].seedFrom) < names.indexOf(name));
    });
    await test('plan.stable-ids', () => {
        assert.deepEqual(Object.keys(catalog).sort(),
            Object.keys(require('../support/regression-cases.json')).sort());
        const ids = selection => createPlan(selection).cases.map(item => item.id).sort();
        assert.deepEqual(ids('files,policy'), ids('policy,files'));
    });
    await test('plan.registry-duplicates', () => {
        const duplicate = {a:{cases:[{id:'test.same',name:'one'}]}, b:{cases:[{id:'test.same',name:'two'}]}};
        assert.throws(() => validateCatalog(duplicate), /Duplicate case ID/);
    });
    await test('plan.execution-identities', () => {
        assertCaseIds(['test.b','test.a'], ['test.a','test.b']);
        for (const ids of [['test.a'], ['test.a','test.b','test.c'], ['test.a','test.a']])
            assert.throws(() => assertCaseIds(ids, ['test.a','test.b']));
    });
    await test('plan.recorder-integrity', async () => {
        const report = {cases:[]};
        const check = createCaseRecorder([{id:'test.bound', name:'bound case'}], report);
        await assert.rejects(check('missing', async () => {}), /Unregistered/);
        await check('bound case', async item => assert.equal(item.id, 'test.bound'));
        await assert.rejects(check('bound case', async () => {}), /Duplicate/);
        const failed = {cases:[]};
        await assert.rejects(createCaseRecorder([{id:'test.failure',name:'failure'}], failed)('failure',
            async () => {throw Error('intentional');}), /intentional/);
        assert.equal(failed.cases[0].result, 'FAIL');
        assert.equal(failed.cases[0].id, 'test.failure');
    });
    await test('plan.report-identities', () => {
        const plan = createPlan('persistence');
        const fixture = {version:4, suite:'persistence', plannedCases:plan.count, result:'PASS', finishedAt:'2026-11-07',
            build:{sourceSha256:'a'.repeat(64),isoSha256:'b'.repeat(64),kernelSha256:'c'.repeat(64)},
            suites:plan.names.map(name => ({name,result:'PASS',checks:catalog[name].checks,
                caseIds:catalog[name].cases.map(item => item.id)})),
            summary:{passed:plan.names.length,failed:0,notRun:0,checks:plan.count}};
        assert(analyze(fixture).passed);
        const wrong = JSON.parse(JSON.stringify(fixture)); wrong.suites[0].caseIds[0] = 'test.forged';
        assert(!analyze(wrong).passed);
        const missing = JSON.parse(JSON.stringify(fixture)); missing.suites[0].caseIds.pop();
        assert(!analyze(missing).passed);
    });
    console.log('PLAN TEST PASS: ' + passed + ' cases');
})().catch(error => {console.error(error);process.exitCode = 1;});
