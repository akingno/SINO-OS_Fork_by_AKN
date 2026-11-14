const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const {execFileSync} = require('node:child_process');
const {tool} = require('../../tools/host-tools');
const names = ['fifo-ownership-and-revoke-96', 'timer-capacity-and-cancel-96',
    'timer-wrap-and-fifo-ties', 'timer-revoke-and-reinsert'].map(n => 'task.queues.' + n);
if (process.argv.length === 3 && process.argv[2] === '--list') {
    console.log(names.join('\n') + '\n4 cases');
} else {
    assert.equal(process.argv.length, 2, 'Only --list is accepted');
    const runs = path.resolve(__dirname, '../../../Build Output/dev/runs');
    fs.mkdirSync(runs, {recursive:true});
    const directory = fs.mkdtempSync(path.join(runs, 'task-queues-'));
    const exe = path.join(directory, 'task-queues.exe');
    const report = {result:'FAIL', cases:[]};
    try {
        execFileSync(tool('g++'), ['-std=c++17','-O2','-Wall','-Wextra','-Werror',
            path.join(__dirname, 'test-task-queues.cpp'), '-o', exe], {windowsHide:true, timeout:30000});
        const output = execFileSync(exe, [], {encoding:'utf8', windowsHide:true, timeout:10000});
        fs.writeFileSync(path.join(directory, 'output.log'), output);
        const ids = output.trim().split(/\r?\n/).map(line => { assert(line.startsWith('PASS ')); return line.slice(5); });
        assert.deepEqual(ids, names);
        report.cases = ids.map(id => ({id, result:'PASS'})); report.result = 'PASS';
        console.log('PASS task queues: 4 cases');
    } catch (e) {
        report.error = e.stack;
        fs.writeFileSync(path.join(directory, 'failure.log'), String(e.stderr || e.stack));
        console.error('FAIL task queues; see failure.log'); process.exitCode = 1;
    } finally {
        fs.writeFileSync(path.join(directory, 'task-queues-report.json'), JSON.stringify(report, null, 2));
        console.log('Report: ' + directory);
    }
}
