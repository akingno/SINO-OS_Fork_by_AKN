/* Real desktop events, checked as text and resources, without pixel matching. */
const assert = require('node:assert/strict');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function run(context, api) {
    const {qmp, guestLog} = context;
    const {check, command, resources, value, until, launch, processes, memory, symbols} = api;
    const baseline = await resources();
    const faults = await value('process_faults');
    async function key(...codes) {
        const epoch = await value('gui_key_epoch');
        await qmp.call('send-key', {keys:codes.map(data => ({type:'qcode', data})), 'hold-time':20});
        await until(async () => await value('gui_key_epoch') !== epoch, 'desktop consumes key');
        await sleep(30);
    }
    async function type(text) {
        for (const c of text) await key(c === '\n' ? 'ret' : c === ' ' ? 'spc' : c);
    }
    async function window(pid) {
        const owners = await memory(symbols.user_owner, 32);
        for (let i = 0; i < 8; i++) if (owners.readUInt32LE(i * 4) === pid) return i;
        return -1;
    }
    async function text(pid) {
        const id = await window(pid);
        if (id < 0) return '';
        await qmp.call('stop');
        try {
            const ptr = (await memory(symbols.user_scene + id * 4, 4)).readUInt32LE();
            if (!ptr) return '';
            const data = await memory(ptr, 4 + 256 * 104);
            const count = data.readUInt32LE();
            assert(count <= 256, 'bounded scene');
            const rows = new Map();
            for (let n = 0; n < count; n++) {
                const c = data.subarray(4 + n * 104, 4 + (n + 1) * 104);
                if (c.readUInt32LE() !== 9) continue;
                const y = c.readInt32LE(8);
                rows.set(y, (rows.get(y) || '') + c.subarray(24,104).toString('utf8').split('\0')[0]);
            }
            return [...rows.values()].map(row => row.trimEnd()).join('\n');
        } finally { await qmp.call('cont'); }
    }
    async function waitText(pid, fragment) {
        await until(async () => (await text(pid)).includes(fragment), 'terminal text ' + fragment);
    }
    async function close(pid) {
        await command('kill ' + pid);
        await until(async () => (await processes()).length === 0,
            'frontend and deadline-bound probe reaped');
        assert.deepEqual(await resources(), baseline);
    }
    let first, second;
    await check('G1 independent Ring 3 frontend and shared text model', async () => {
        const offset = guestLog().length;
        const pid = await launch('term', '/selftest');
        await until(() => guestLog().slice(offset).includes('TERMINAL MODEL PASS'), 'frontend text selftest');
        await until(async () => !(await processes()).some(p => p.pid === pid), 'model selftest exit');
        assert.deepEqual(await resources(), baseline);
        await key('f12');
        await until(async () => (await processes()).filter(p => p.name === 'term.snx').length === 2, 'F12 frontend and strict probe');
        first = (await processes()).filter(p => p.name === 'term.snx')[0].pid;
        await waitText(first, 'G1 session probe');
        await type('ab'); await key('backspace'); await type('c\n');
        await waitText(first, 'LINE ac');
        assert(!(await text(first)).includes('LINE abc'));
    });
    await check('G1 two focused windows isolate input and IME commits', async () => {
        second = await launch('term', '/qa');
        await waitText(second, 'G1 session probe');
        await type('second\n'); await waitText(second, 'LINE second');
        assert(!(await text(first)).includes('second'));
        await key('ctrl','spc'); await type('nihao'); await key('spc'); await key('ret');
        await waitText(second, 'LINE 你好');
        assert(!(await text(second)).includes('LINE nihao'));
        // Pending IME text must be discarded when keyboard focus changes.
        await type('ni'); await key('alt','tab'); await key('spc'); await key('ctrl','spc');
        await type('x\n');
        await waitText(first, 'LINE  x');
        assert(!(await text(first)).includes('你'));
        await type('first\n'); await waitText(first, 'LINE first');
        assert(!(await text(second)).includes('LINE first'));
    });
    await check('G1 raw keys, UTF-8 output, controls and output backpressure', async () => {
        await type('raw\n'); await waitText(first, 'RAW ready');
        await key('up'); await waitText(first, 'KEY 328');
        await type('z'); await waitText(first, 'BYTE 122');
        await key('esc'); await waitText(first, 'LINE ready');
        await type('flood\n'); await waitText(first, 'FLOOD DONE 12289');
        await type('unicode\n'); await waitText(first, '你好?X?Y');
        await type('rawpause\n');
        await until(async () => (await text(first)).endsWith('RAW ready\n') ||
            (await text(first)).endsWith('RAW ready'), 'paused raw probe');
        for (let i = 0; i < 12; i++) await key(i % 2 ? 'down' : 'up');
        await waitText(first, Array.from({length:12}, (_, i) => 'KEY ' + (i % 2 ? 336 : 328)).join('\n'));
        await key('esc');
    });
    await check('G1 simple window size mapping and coalesced session resize', async () => {
        // One desktop maximize/restore operation, no scaling or pixel checks.
        const id = await window(first);
        const stride = await value('gui_window_stride');
        const b = await memory(symbols.windows + id * stride, 28);
        const x = b.readInt32LE(8), y = b.readInt32LE(12), w = b.readInt32LE(16);
        await qmp.click(x + w - 16, y + 15);
        await waitText(first, 'RESIZE 120 35');
    });
    await check('G1 directed Ctrl+C leaves the other session alive', async () => {
        await key('ctrl','c'); await waitText(first, 'Task canceled');
        assert((await processes()).some(p => p.pid === second));
        await key('ctrl','w');
        await until(async () => await window(first) < 0, 'normal frontend close');
        await key('ctrl','d'); await waitText(second, 'Task exited 0');
        await key('ctrl','w');
        await until(async () => (await processes()).length === 0, 'both sessions closed');
        assert.deepEqual(await resources(), baseline);
    });
    await check('G1 frontend fault and controller loss reclaim bounded probe', async () => {
        const offset = guestLog().length;
        first = await launch('term', '/fault-test');
        await until(async () => await value('process_faults') === faults + 1, 'frontend faults once');
        await until(async () => (await processes()).length === 0, 'faulted frontend probe deadline');
        assert(guestLog().slice(offset).includes('USER fault contained'), 'real CPL3 fault');
        assert.deepEqual(await resources(), baseline);
        first = await launch('term', '/qa'); await waitText(first, 'G1 session probe');
        await close(first);
    });
    await check('G1 client fault and external termination display typed results', async () => {
        const before = await value('process_faults');
        first = await launch('term', '/qa'); await waitText(first, 'G1 session probe');
        await type('fault\n'); await waitText(first, 'Task faulted');
        assert.equal(await value('process_faults'), before + 1);
        await key('ctrl','w'); await until(async () => (await processes()).length === 0, 'client fault window closes');
        assert.deepEqual(await resources(), baseline);
        first = await launch('term', '/qa'); await waitText(first, 'G1 session probe');
        const children = (await processes()).filter(p => p.pid !== first);
        assert.equal(children.length, 1);
        await command('kill ' + children[0].pid); await waitText(first, 'Task terminated');
        await key('ctrl','w'); await until(async () => (await processes()).length === 0, 'terminated client window closes');
        assert.deepEqual(await resources(), baseline);
    });
    await check('G1 session exhaustion fails cleanly and repeated close recovers', async () => {
        first = await launch('term', '/qa'); await waitText(first, 'G1 session probe');
        second = await launch('term', '/qa'); await waitText(second, 'G1 session probe');
        const offset = guestLog().length;
        const third = await launch('term', '/qa');
        await until(() => guestLog().slice(offset).includes('TERMINAL FRONTEND unavailable'), 'session quota failure');
        await until(async () => !(await processes()).some(p => p.pid === third), 'failed frontend reclaimed');
        await key('ctrl','w'); await until(async () => await window(second) < 0, 'second normal close');
        await key('ctrl','w'); await until(async () => (await processes()).length === 0, 'first normal close');
        assert.deepEqual(await resources(), baseline);
        assert.equal(await value('process_faults'), faults + 2);
    });
    await check('G1 full pending input remains closable without waiting for the client', async () => {
        first = await launch('term', '/qa'); await waitText(first, 'G1 session probe');
        await type('stall\n'); await waitText(first, 'RAW ready');
        const offset = guestLog().length;
        for (let i = 0; i < 80; i++) await key('up');
        assert(guestLog().slice(offset).includes('G1 INPUT FULL'), 'actually exhausted the pending queue');
        await key('ctrl','w');
        await until(async () => (await processes()).length === 0, 'close bypasses input pressure', 2000);
        assert.deepEqual(await resources(), baseline);
    });
    await check('G1 old F2 desktop Shell still accepts keyboard input', async () => {
        await key('f2');
        const stride = await value('gui_window_stride');
        await until(async () => {
            const windows = await memory(symbols.windows, stride * 8);
            for (let i = 0; i < 8; i++) if (windows.readUInt32LE(i * stride) &&
                windows.readUInt32LE(i * stride + 4) === 6) return true;
            return false;
        }, 'legacy terminal window');
        await type('echo legacyready\n');
        await until(async () => {
            const cells = await memory(symbols.terminal, 80 * 25 * 4);
            const rows = [];
            let output = '';
            for (let i = 0; i < cells.length; i += 4) {
                const cp = cells.readUInt32LE(i) & 0x1fffff;
                output += cp <= 0x10ffff ? String.fromCodePoint(cp) : ' ';
                if ((i + 4) % (80 * 4) === 0) { rows.push(output.trim()); output = ''; }
            }
            return rows.includes('legacyready');
        }, 'legacy desktop output');
        await key('ctrl','w');
        assert.deepEqual(await resources(), baseline);
    });
}
module.exports = {run};
