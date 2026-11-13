const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('../../tools/vm-config');
const cases = [
    ['vm.config.defaults', () => {
        assert.deepEqual(vm.args(), ['-smp', '1', '-m', '4096M']);
        assert.deepEqual(vm.args(1024), ['-smp', '1', '-m', '1024M']);
        assert.deepEqual(vm.args(2048), ['-smp', '1', '-m', '2048M']);
    }],
    ['vm.config.reject-invalid-memory', () => {
        for (const value of [64, 128, 0, -1, 4096.5, '4096', NaN, 8192]) {
            assert.throws(() => vm.args(value), /Unsupported/);
        }
    }],
    ['vm.config.qmp-identity', async () => {
        const qmp = (bytes, cpus = 1, plugged = 0) => ({call:async command => {
            if (command === 'query-memory-size-summary') return {'base-memory':bytes, 'plugged-memory':plugged};
            assert.equal(command, 'query-cpus-fast');
            return Array.from({length:cpus}, (_, i) => ({'cpu-index':i}));
        }});
        assert.deepEqual(await vm.verify(qmp(4294967296)), {memoryMiB:4096, cpuCount:1, baseMemoryBytes:4294967296});
        await assert.rejects(vm.verify(qmp(1073741824)), /RAM identity/);
        await assert.rejects(vm.verify(qmp(4294967296, 2)), /single-core identity/);
        await assert.rejects(vm.verify(qmp(4294967296, 1, 4096)), /hotplugged/);
    }],
    ['vm.config.entry-points', () => {
        const root = path.resolve(__dirname, '../..');
        for (const name of ['tools/dev-run.js', 'tools/shell-test.js', 'tests/host/test-boot.js', 'tests/host/test-console.js']) {
            const text = fs.readFileSync(path.join(root, name), 'utf8');
            assert.match(text, /vm\.args\(/, name + ': shared arguments');
            assert.match(text, /vm\.verify\(/, name + ': live QMP identity');
            assert.doesNotMatch(text, /['"]64M['"]/, name + ': obsolete default');
        }
        const release = fs.readFileSync(path.join(root, 'release/launch.ps1'), 'utf8');
        assert.match(release, new RegExp('\\$memoryMiB = ' + vm.memoryMiB + '\\b'));
        assert.match(release, new RegExp('\\$cpuCount = ' + vm.cpuCount + '\\b'));
        assert.match(release, /'-smp',\(\[string\]\$cpuCount\),'-m',\(\$memoryMiB/);
    }]
];
async function main() {
    if (process.argv.length === 3 && process.argv[2] === '--list') {
        console.log(cases.map(([id]) => id).join('\n') + '\n4 cases');
        return;
    }
    assert.equal(process.argv.length, 2, 'Only --list is accepted');
    const root = path.resolve(__dirname, '../../..'), directory = path.join(root, 'Build Output/phase00a');
    fs.mkdirSync(directory, {recursive:true});
    const report = {result:'FAIL', cases:[]};
    try {
        for (const [id, run] of cases) {
            const item = {id, result:'FAIL'}; report.cases.push(item);
            await run(); item.result = 'PASS';
        }
        report.result = 'PASS';
        console.log('PASS VM config: 4 cases');
    } finally {
        fs.writeFileSync(path.join(directory, 'vm-config-report.json'), JSON.stringify(report, null, 2));
    }
}
main().catch(error => {console.error(error);process.exitCode = 1;});
