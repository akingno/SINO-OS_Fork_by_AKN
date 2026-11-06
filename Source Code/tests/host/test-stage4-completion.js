const {runPhase} = require('../support/stage4-wait-tests');

runPhase('C', [
    ['legacy completion', 'comptest', 'suite', 'COMPLETION SUITE PASS'],
    ['legacy managed run', 'clitest', 'unit', 'CLI SUITE PASS'],
    ...['exit', 'run-exit', 'run-timeout', 'timeout', 'multi-cancel', 'local-close', 'unmap', 'wire']
        .map(mode => [mode, 'comptest', 'w:' + mode, 'COMPLETION BLOCK PASS'])
], ['read-wake', 'contended-read']);
