const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {execFileSync} = require('node:child_process');
const {tool} = require('../../../../tools/host-tools');

const profiles = [
    {name:'normal', flags:[], cases:[
        'dense-small-allocations', 'full-slab-reuse', 'mixed-size-classes',
        'lifo-and-release-validation', 'direct-mappings', 'zeroed-allocation-and-bounds'
    ]},
    {name:'debug', flags:['-DHEAP_DEBUG'], cases:['release-scrub', 'allocation-scrub']}
];
const caseId = name => 'user.heap.' + name;
const names = profiles.flatMap(profile => profile.cases.map(caseId));

if (process.argv.length === 3 && process.argv[2] === '--list') {
    console.log(names.join('\n') + '\n' + names.length + ' cases');
} else {
    assert.equal(process.argv.length, 2, 'Only --list is accepted');
    const runs = path.resolve(__dirname, '../../../../../Build Output/dev/runs');
    fs.mkdirSync(runs, {recursive:true});
    const directory = fs.mkdtempSync(path.join(runs, 'user-heap-'));
    const report = {result:'FAIL', cases:[]};
    const options = {encoding:'utf8', windowsHide:true, timeout:30000, stdio:['ignore', 'pipe', 'pipe']};
    const source = path.join(__dirname, 'test-user-heap.cpp');
    try {
        for (const profile of profiles) {
            const exe = path.join(directory, profile.name + '.exe');
            execFileSync(tool('g++'), ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                ...profile.flags, source, '-o', exe], options);
            const output = execFileSync(exe, [], options);
            fs.writeFileSync(path.join(directory, profile.name + '.log'), output);
            const expected = profile.cases.map(caseId);
            assert.deepEqual(output.trim().split(/\r?\n/), expected.map(id => 'PASS ' + id));
            report.cases.push(...expected.map(id => ({id, result:'PASS'})));
        }
        report.result = 'PASS';
        console.log('PASS user heap: ' + names.length + ' cases');
    } catch (error) {
        report.error = error.message;
        fs.writeFileSync(path.join(directory, 'failure.log'), String(error.stderr || error.stack));
        console.error('FAIL user heap; see failure.log');
        process.exitCode = 1;
    } finally {
        fs.writeFileSync(path.join(directory, 'user-heap-report.json'), JSON.stringify(report, null, 2));
        console.log('Report: ' + directory);
    }
}
