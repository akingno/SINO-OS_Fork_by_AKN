/* Profiles compose leaf suites; leaf suites never call one another. */
const native = ['process', 'process-reboot', 'policy', 'completion', 'cli', 'files',
    'files-reboot', 'pipes', 'terminal', 'interactive', 'network'];
const profiles = {
    core: ['infrastructure', 'rtc-driver', 'shell-transport', 'shell-commands',
        ...native, 'managed-cli', 'persistence-write', 'persistence-read', 'runner-rejects-failure',
        'pipe-waits', 'completion-waits', 'terminal-waits', 'shell-waits'],
    process: ['process', 'process-reboot'],
    files: ['files', 'files-reboot'],
    persistence: ['persistence-write', 'persistence-read'],
    stress: ['process-stress']
};
for (const name of [...native, 'infrastructure', 'rtc-driver', 'shell-transport',
    'shell-commands', 'managed-cli', 'runner-rejects-failure', 'process-stress',
    'pipe-waits', 'completion-waits', 'terminal-waits', 'shell-waits']) {
    if (!profiles[name] && !name.endsWith('-reboot'))
        profiles[name] = [name];
}
module.exports = profiles;
