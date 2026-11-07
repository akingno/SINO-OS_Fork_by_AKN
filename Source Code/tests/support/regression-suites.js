/* Profiles compose leaf suites; leaf suites never call one another. */
const native = ['process', 'process-reboot', 'policy', 'completion', 'cli', 'files',
    'files-reboot', 'pipes', 'terminal', 'interactive', 'network'];
const gui = ['files-gui', 'frontend-gui', 'shell-gui', 'file-tools-gui', 'shell-tools-gui',
    'workspace-gui', 'line-edit-gui', 'gui-persistence-write', 'gui-persistence-read'];
const profiles = {
    core: ['infrastructure', 'rtc-driver', 'shell-transport', 'shell-commands',
        ...native, 'managed-cli', 'persistence-write', 'persistence-read', 'runner-rejects-failure'],
    process: ['process', 'process-reboot'],
    files: ['files', 'files-reboot'],
    persistence: ['persistence-write', 'persistence-read'],
    'terminal-gui': gui,
    'workspace-gui': ['workspace-gui', 'gui-persistence-write', 'gui-persistence-read']
};
for (const name of [...native, ...gui, 'infrastructure', 'rtc-driver', 'shell-transport',
    'shell-commands', 'managed-cli', 'runner-rejects-failure']) {
    if (!profiles[name] && !name.endsWith('-reboot') && !name.startsWith('gui-persistence-'))
        profiles[name] = [name];
}
module.exports = profiles;
