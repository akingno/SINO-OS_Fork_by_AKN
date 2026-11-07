/* Focused probes; legacy full suites belong to their functional owners. */
const {checkedProbe, pipeModes, quietShell} = require('../../support/wait-probes');
async function pipes(api) {
    await pipeModes(api, ['read-wake', 'write-wake', 'read-eof', 'write-closed', 'read-timeout',
        'contended-read', 'local-close', 'unmap', 'wire']);
    await checkedProbe(api, 'blocked process timeout releases pipe', 'pipetest', 'block-release',
        /PIPE BLOCK CHILD ENTER[\s\S]*PIPE BLOCK RELEASE PASS/);
}
async function completion(api) {
    for (const mode of ['exit', 'run-exit', 'run-timeout', 'timeout', 'multi-cancel', 'local-close', 'unmap', 'wire'])
        await checkedProbe(api, mode, 'comptest', 'w:' + mode, /COMPLETION BLOCK PASS/);
}
async function terminal(api) {
    for (const mode of ['raw', 'canonical', 'eof', 'hangup', 'controller', 'timeout', 'foreground',
        'snapshot', 'unmap', 'local-close', 'wire'])
        await checkedProbe(api, mode, 'termtest', 'b:' + mode, /TERMINAL BLOCK PASS/);
}
async function shell(api) {
    await checkedProbe(api, 'headless Shell result wait', 'brokerqa', 'shell-wait', /SHELL PASS/, {observe:quietShell});
    await checkedProbe(api, 'headless Shell broker failure', 'brokerqa', 'shell-error', /SHELL PASS/);
    await checkedProbe(api, 'SDK terminal read wait', 'termtest', 'b:sdk-read', /TERMINAL BLOCK PASS/);
    await checkedProbe(api, 'thread slot reuse', 'poltest', 'paging-a', /PAGING STRICT A PASS/, {strict:true});
    await checkedProbe(api, 'Shell broker wait interrupted by hangup', 'brokerqa', 'shell-hangup', /SHELL WAIT HANGUP PASS/);
}
module.exports = {pipes, completion, terminal, shell};
