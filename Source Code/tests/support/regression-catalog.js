/* Only this catalog and the runner compose business suites. Imports are lazy. */
const leaf = (name, method = 'run') => api => require('../host/suites/' + name)[method](api);
const guiLeaf = (name, method = 'run') => api => require('../host/' + name)[method](api);
const snapshot = name => records => require('../host/suites/' + name).snapshot(records);
const catalog = {
    infrastructure: {host:leaf('host-contracts', 'infrastructure')},
    'rtc-driver': {host:leaf('host-contracts', 'rtc')},
    'shell-transport': {shell:'transport', checks:25},
    'shell-commands': {shell:'commands', checks:112},
    process: {run:leaf('process'), checks:15, snapshot:snapshot('process')},
    'process-reboot': {run:leaf('process-reboot'), checks:1, seedFrom:'process'},
    policy: {run:leaf('policy'), checks:2},
    completion: {run:leaf('completion'), checks:5},
    cli: {run:leaf('cli'), checks:1},
    files: {run:leaf('files'), checks:1, snapshot:snapshot('files')},
    'files-reboot': {run:leaf('files-reboot'), checks:1, seedFrom:'files'},
    pipes: {run:leaf('pipes'), checks:1},
    terminal: {run:leaf('terminal'), checks:1},
    interactive: {run:leaf('interactive'), checks:1},
    network: {run:leaf('network'), checks:1},
    'managed-cli': {run:leaf('managed-cli'), checks:12},
    'persistence-write': {run:leaf('persistence', 'write'), checks:2, snapshot:snapshot('persistence')},
    'persistence-read': {run:leaf('persistence', 'read'), checks:2, seedFrom:'persistence-write'},
    'runner-rejects-failure': {run:leaf('runner-failure'), checks:1, expectedFailure:true},
    'files-gui': {run:leaf('files-gui'), checks:1, gui:true},
    'frontend-gui': {run:guiLeaf('terminal-gui'), checks:12, gui:true},
    'shell-gui': {run:guiLeaf('shell-gui'), checks:5, gui:true},
    'file-tools-gui': {run:guiLeaf('file-tools-gui'), checks:4, gui:true},
    'shell-tools-gui': {run:guiLeaf('shell-tools-gui'), checks:4, gui:true},
    'workspace-gui': {run:guiLeaf('workspace-gui'), checks:3, gui:true},
    'line-edit-gui': {run:guiLeaf('line-edit-gui'), checks:1, gui:true},
    'gui-persistence-write': {run:guiLeaf('workspace-gui', 'persist'), checks:1, gui:true,
        snapshot:snapshot('gui-persistence')},
    'gui-persistence-read': {run:leaf('gui-persistence', 'read'), checks:1, gui:true,
        seedFrom:'gui-persistence-write'}
};
module.exports = catalog;
