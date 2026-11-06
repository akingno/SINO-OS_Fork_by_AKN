const assert = require('node:assert/strict');
const {VM, sleep} = require('./vm-test');
const path = require('node:path');
const fs = require('node:fs');
const {volume} = require('./test-volume');
const {expectInput} = require('./files-test');

async function waitFor(predicate, description) {
    const deadline = Date.now() + 5000;
    while (Date.now() < deadline) {
        const result = await predicate();
        if (result) return result;
        await sleep(40);
    }
    throw Error(description);
}

async function text(vm, id, legacy) {
    await vm.q.call('stop');
    try {
        if (legacy) {
            const cells = await vm.memory(vm.symbols.terminal, 80 * 26 * 4);
            return Array.from({length: 26}, (_, y) =>
                Array.from({length: 80}, (_, x) => {
                    const cp = cells.readUInt32LE((y * 80 + x) * 4) & 0x1fffff;
                    return cp > 0 && cp <= 0x10ffff ? String.fromCodePoint(cp) : ' ';
                }).join('').trimEnd()).join('\n');
        }
        const ptr = (await vm.memory(vm.symbols.user_scene + id * 4, 4)).readUInt32LE();
        if (!ptr) return '';
        const data = await vm.memory(ptr, 4 + 256 * 104);
        const rows = new Map();
        for (let i = 0; i < data.readUInt32LE(); i++) {
            const cell = data.subarray(4 + i * 104, 108 + i * 104);
            if (cell.readUInt32LE() !== 9) continue;
            const y = cell.readInt32LE(8);
            rows.set(y, (rows.get(y) || '') + cell.subarray(24).toString().split('\0')[0]);
        }
        return [...rows.values()].map(row => row.trimEnd()).join('\n');
    } finally {
        await vm.q.call('cont');
    }
}

async function shellCommand(vm, win, line, expected = '') {
    const ready = screen => /gsh:\/[^\n]* \$$/.test(screen);
    await waitFor(async () => ready(await text(vm, win.id, false)), 'Shell ready');
    await vm.type(line + '\n');
    return waitFor(async () => {
        const screen = await text(vm, win.id, false);
        const marker = '$ ' + line + '\n';
        const at = screen.lastIndexOf(marker);
        return at >= 0 && ready(screen) && screen.slice(at + marker.length).includes(expected)
            ? screen : false;
    }, 'Shell completes ' + line);
}

async function contract(vm, legacy, open, comparison) {
    const app = legacy ? 6 : 9;
    await open();
    const win = await waitFor(async () => (await vm.state()).windows.find(w =>
        w.app === app && (legacy || w.title === 'SINO Shell')), 'terminal opens');
    const screen = () => text(vm, win.id, legacy);
    const prompt = () => waitFor(async () => (await screen()).trimEnd().endsWith('$'), 'ready prompt');
    async function output(command, expected) {
        await vm.type(command + '\n');
        await waitFor(async () => (await screen()).includes(expected), command + ': ' + expected);
        await prompt();
    }
    await prompt();
    await vm.type('echo ediing');
    for (let i = 0; i < 3; i++) await vm.key('left');
    await vm.type('t\n');
    await waitFor(async () => (await screen()).includes('\nediting\n'), 'cursor insertion');
    await prompt();
    await vm.key('up');
    await vm.key('ret');
    await waitFor(async () => ((await screen()).match(/\nediting\n/g) || []).length === 2, 'history recall');
    await output('cat welcome.txt', '\nWelcome to SINO Platinum Desktop.');
    await vm.type('echo discard');
    await vm.key('ctrl', 'c');
    await output('echo recovered', '\nrecovered\n');
    if (comparison) {
        const path = legacy ? 'g6-legacy.txt' : 'g6-new.txt';
        await vm.type('echo persisted > ' + path + '\n');
        await prompt();
        await output('cat ' + path, legacy ? '\npersisted\n' : 'file error');
    } else {
        await vm.type('echo persisted > /home/user/g6-persist.txt\n');
        await prompt();
        await shellCommand(vm, win, 'cp /boot/first.bas g6-first.bas', 'published in RAM');
        await output('sync', 'home snapshot durable');
        await output('cat g6-persist.txt', '\npersisted\n');
        await output('head -n 1 /boot/readme.txt', 'SINO 0.10 Desktop and Hard-Disk Installation');
        await vm.type('install /quiet:/boot/hello.sxp\n');
        await waitFor(() => vm.log().includes('QUIET INSTALL PASS'), 'installer through Ring 3 Shell');
        await prompt();
        await vm.type('run /boot/probe.snx\n');
        await waitFor(() => vm.log().includes('RING3 PROBE PASS'), 'Ring 3 launch');
        const probe = await waitFor(async () => (await vm.state()).windows.find(w =>
            w.title.includes('Ring 3')), 'probe window');
        await vm.click(probe.x + 70, probe.y + 173);
        await waitFor(async () => (await vm.state()).focus === win.id, 'probe closes to Shell');
    }
    await vm.key('ctrl', 'w');
    await waitFor(async () => !(await vm.state()).windows.some(w => w.id === win.id), 'terminal closes');
}

async function desktopEntry(vm, open, writable) {
    await open();
    const win = await waitFor(async () => (await vm.state()).windows.find(w =>
        w.title === 'SINO Shell'), 'Ring 3 terminal entry');
    assert.equal(win.app, 9);
    const screen = () => text(vm, win.id, false);
    await waitFor(async () => (await screen()).includes(writable ? '(read-write)' : '(read-only)'),
        'expected workspace mode');
    await waitFor(async () => (await screen()).endsWith('gsh:/ $'), 'entry ready');
    if (!writable) {
        await vm.type('echo denied > g6-denied.txt\n');
        await waitFor(async () => (await screen()).includes('read-only session'), 'write refused');
        await vm.type('run /boot/spin.snx\n');
        await waitFor(async () => (await screen()).match(/read-only session/g)?.length === 2,
            'application launch refused');
    }
    await vm.key('ctrl', 'w');
    await waitFor(async () => !(await vm.state()).windows.some(w => w.id === win.id), 'entry closes');
}

async function main() {
    const comparison = process.argv.includes('--compare-baseline');
    const vm = new VM(comparison ? 'g6-baseline' : 'g6-terminal');
    try {
        await vm.start({fresh: true});
        if (comparison) {
            await contract(vm, true, () => vm.key('f2'), true);
            console.log('PASS old Terminal: keyboard, history, file read, cancel, redirect, close');
            await vm.key('f2');
            await contract(vm, false, () => vm.type('run /boot/term.snx /write\n'), true);
            console.log('PASS new Shell baseline: common behavior, missing redirect reproduced');
        } else {
            assert.equal(vm.symbols.shell_key, undefined, 'old interactive terminal absent');
            assert.equal(vm.symbols.shell_bridge_poll, undefined, 'ordinary image lacks the development bridge');
            await contract(vm, false, () => vm.key('f2'), false);
            console.log('PASS F2 Ring 3 Shell and desktop command contract');
            const records = volume(path.join(vm.run, 'data.img')).current.records;
            assert.equal(records.get('/home/user/g6-persist.txt').data.toString(), 'persisted\n');
            assert.deepEqual(records.get('/home/user/g6-first.bas').data,
                fs.readFileSync(path.join(__dirname, '..', 'disk32', 'FIRST.BAS')));
            await desktopEntry(vm, async () => {
                await vm.click(22, 13);
                await vm.click(46, 48 + 5 * 28);
            }, true);
            await desktopEntry(vm, async () => {
                await vm.key('ctrl', 'o');
                await expectInput(vm);
                await vm.type('/Applications/Terminal.app\n');
            }, true);
            await desktopEntry(vm, () => vm.key('f12'), false);
            console.log('PASS SINO menu, Terminal.app, read-only F12 and durable file bytes');
        }
    } finally {
        await vm.stop();
    }
}

if (require.main === module) main().catch(error => {
    console.error(error.message);
    process.exitCode = 1;
});
module.exports = {terminalText: text, shellCommand};
