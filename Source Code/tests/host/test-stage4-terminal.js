const {runPhase} = require('../support/stage4-wait-tests');

runPhase('D', [
    ['legacy terminal', 'termtest', 'suite', 'TERMINAL SUITE PASS'],
    ...['raw', 'canonical', 'eof', 'hangup', 'controller', 'timeout', 'foreground',
        'snapshot', 'unmap', 'local-close', 'wire']
        .map(mode => [mode, 'termtest', 'b:' + mode, 'TERMINAL BLOCK PASS']),
    ['completion wake', 'comptest', 'w:exit', 'COMPLETION BLOCK PASS']
], ['read-wake']);
