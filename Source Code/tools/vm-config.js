const assert = require('node:assert/strict');

const memoryMiB = 4096;
const cpuCount = 1;
function args(memory = memoryMiB) {
    assert([1024, 2048, 4096].includes(memory), 'Unsupported test VM memory size.');
    return ['-smp', String(cpuCount), '-m', `${memory}M`];
}
async function verify(qmp, memory = memoryMiB) {
    args(memory);
    const size = await qmp.call('query-memory-size-summary');
    const cpus = await qmp.call('query-cpus-fast');
    assert.equal(size['base-memory'], memory * 1024 * 1024, 'QMP RAM identity');
    assert.equal(size['plugged-memory'] || 0, 0, 'Unexpected hotplugged RAM');
    assert.equal(cpus.length, cpuCount, 'QMP single-core identity');
    assert.equal(cpus[0]['cpu-index'], 0, 'Unexpected CPU index');
    return {memoryMiB:memory, cpuCount, baseMemoryBytes:size['base-memory']};
}
module.exports = {memoryMiB, cpuCount, args, verify};
