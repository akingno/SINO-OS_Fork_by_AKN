/* Only this catalog and the runner compose business suites. Imports are lazy. */
const leaf = (name, method = 'run') => api => require('../host/suites/' + name)[method](api);
const guiLeaf = (name, method = 'run') => api => require('../host/' + name)[method](api);
const snapshot = name => records => require('../host/suites/' + name).snapshot(records);
const catalog = {
    infrastructure: {host:leaf('host-contracts', 'infrastructure')},
    'rtc-driver': {host:leaf('host-contracts', 'rtc')},
    'shell-transport': {shell:'transport'},
    'shell-commands': {shell:'commands'},
    process: {run:leaf('process'), snapshot:snapshot('process')},
    'process-stress': {run:leaf('process-stress')},
    'process-reboot': {run:leaf('process-reboot'), seedFrom:'process'},
    policy: {run:leaf('policy')},
    completion: {run:leaf('completion')},
    cli: {run:leaf('cli')},
    files: {run:leaf('files'), snapshot:snapshot('files')},
    'files-reboot': {run:leaf('files-reboot'), seedFrom:'files'},
    pipes: {run:leaf('pipes')},
    terminal: {run:leaf('terminal')},
    interactive: {run:leaf('interactive')},
    network: {run:leaf('network')},
    'managed-cli': {run:leaf('managed-cli')},
    'persistence-write': {run:leaf('persistence', 'write'), snapshot:snapshot('persistence')},
    'persistence-read': {run:leaf('persistence', 'read'), seedFrom:'persistence-write'},
    'runner-rejects-failure': {run:leaf('runner-failure'), expectedFailure:true},
    'pipe-waits': {run:leaf('waits', 'pipes')},
    'completion-waits': {run:leaf('waits', 'completion')},
    'terminal-waits': {run:leaf('waits', 'terminal')},
    'shell-waits': {run:leaf('waits', 'shell')},
    'files-gui': {run:leaf('files-gui'), gui:true},
    'frontend-gui': {run:guiLeaf('terminal-gui'), gui:true},
    'shell-gui': {run:guiLeaf('shell-gui'), gui:true},
    'file-tools-gui': {run:guiLeaf('file-tools-gui'), gui:true},
    'shell-tools-gui': {run:guiLeaf('shell-tools-gui'), gui:true},
    'workspace-gui': {run:guiLeaf('workspace-gui'), gui:true},
    'line-edit-gui': {run:guiLeaf('line-edit-gui'), gui:true},
    'gui-persistence-write': {run:guiLeaf('workspace-gui', 'persist'), gui:true,
        snapshot:snapshot('gui-persistence')},
    'gui-persistence-read': {run:leaf('gui-persistence', 'read'), gui:true,
        seedFrom:'gui-persistence-write'}
};
const manifest = require('./regression-cases.json');
for (const [name, definition] of Object.entries(catalog)) {
    definition.cases = manifest[name];
    if (!definition.cases?.length) throw Error('Missing case manifest: ' + name);
    definition.checks = definition.cases.length;
}
module.exports = catalog;
