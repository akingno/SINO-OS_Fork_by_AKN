/* Terminal frontend integration only. Other GUI suites are selected by the runner. */
const assert = require('node:assert/strict');
async function run(api) {
    const {qmp, guestLog, check, command, resources, value, until, launch,
        processes, memory, symbols, key, type, window, text, waitText, close} = api;
    const baseline = await resources();
    const faults = await value('process_faults');
    let first, second;
    await check('G1 independent Ring 3 frontend and shared text model', async () => {
        const offset = guestLog().length;
        const pid = await launch('termqa', '/selftest');
        await until(() => guestLog().slice(offset).includes('TERMINAL MODEL PASS'), 'frontend text selftest');
        await until(async () => !(await processes()).some(p => p.pid === pid), 'model selftest exit');
        assert.deepEqual(await resources(), baseline);
        await launch('termqa');
        await until(async () => (await processes()).filter(p => p.name === 'termqa.snx').length === 2, 'F12 frontend and strict probe');
        first = (await processes()).filter(p => p.name === 'termqa.snx')[0].pid;
        await waitText(first, 'G1 session probe');
        await type('ab'); await key('backspace'); await type('c\n');
        await waitText(first, 'LINE ac');
        assert(!(await text(first)).includes('LINE abc'));
    });
    await check('G1 two focused windows isolate input and IME commits', async () => {
        second = await launch('termqa', '/qa');
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
        first = await launch('termqa', '/fault-test');
        await until(async () => await value('process_faults') === faults + 1, 'frontend faults once');
        await until(async () => (await processes()).length === 0, 'faulted frontend probe deadline');
        assert(guestLog().slice(offset).includes('USER fault contained'), 'real CPL3 fault');
        assert.deepEqual(await resources(), baseline);
        first = await launch('termqa', '/qa'); await waitText(first, 'G1 session probe');
        await close(first);
    });
    await check('G1 client fault and external termination display typed results', async () => {
        const before = await value('process_faults');
        first = await launch('termqa', '/qa'); await waitText(first, 'G1 session probe');
        await type('fault\n'); await waitText(first, 'Task faulted');
        assert.equal(await value('process_faults'), before + 1);
        await key('ctrl','w'); await until(async () => (await processes()).length === 0, 'client fault window closes');
        assert.deepEqual(await resources(), baseline);
        first = await launch('termqa', '/qa'); await waitText(first, 'G1 session probe');
        const children = (await processes()).filter(p => p.pid !== first);
        assert.equal(children.length, 1);
        await command('kill ' + children[0].pid); await waitText(first, 'Task terminated');
        await key('ctrl','w'); await until(async () => (await processes()).length === 0, 'terminated client window closes');
        assert.deepEqual(await resources(), baseline);
    });
    await check('G1 session exhaustion fails cleanly and repeated close recovers', async () => {
        first = await launch('termqa', '/qa'); await waitText(first, 'G1 session probe');
        second = await launch('termqa', '/qa'); await waitText(second, 'G1 session probe');
        const offset = guestLog().length;
        const third = await launch('termqa', '/qa');
        await until(() => guestLog().slice(offset).includes('TERMINAL FRONTEND unavailable'), 'session quota failure');
        await until(async () => !(await processes()).some(p => p.pid === third), 'failed frontend reclaimed');
        await key('ctrl','w'); await until(async () => await window(second) < 0, 'second normal close');
        await key('ctrl','w'); await until(async () => (await processes()).length === 0, 'first normal close');
        assert.deepEqual(await resources(), baseline);
        assert.equal(await value('process_faults'), faults + 2);
    });
    await check('G1 full pending input remains closable without waiting for the client', async () => {
        first = await launch('termqa', '/qa'); await waitText(first, 'G1 session probe');
        await type('stall\n'); await waitText(first, 'RAW ready');
        const offset = guestLog().length;
        for (let i = 0; i < 80; i++) await key('up');
        assert(guestLog().slice(offset).includes('TERMINAL INPUT FULL'), 'actually exhausted the pending queue');
        await key('ctrl','w');
        await until(async () => (await processes()).length === 0, 'close bypasses input pressure', 2000);
        assert.deepEqual(await resources(), baseline);
    });
    await check('G6 F2 uses a writable Ring 3 Shell and the detached app route', async () => {
        await key('f2');
        await until(async () => (await processes()).some(p => p.name === 'term.snx'), 'F2 frontend');
        const pid = (await processes()).find(p => p.name === 'term.snx').pid;
        await waitText(pid, '/ = /home/user (read-write)');
        const prompt = () => until(async () => (await text(pid)).endsWith('gsh:/ $'), 'F2 ready');
        await prompt();
        await type('echo g6-first > /home/user/g6-test.txt\n'); await prompt();
        await type('echo -n g6-second >> g6-test.txt\n'); await prompt();
        assert.equal(await command('cat /home/user/g6-test.txt'), 'g6-first\ng6-second');
        await type('head -n 1 /boot/readme.txt\n');
        await waitText(pid, 'SINO 0.10 Desktop');
        await prompt();
        await type('cp /boot/readme.txt g6-boot.txt\n'); await prompt();
        assert.equal(await command('cat /home/user/g6-boot.txt'), await command('cat /boot/readme.txt'));
        await command('rm /home/user/g6-boot.txt');
        await type('echo denied > /boot/readme.txt\n'); await waitText(pid, 'path outside grant');
        await prompt();
        await type('run /boot/no-such.snx\n'); await waitText(pid, 'start failed -4');
        await prompt();
        await type('run /boot/spin.snx\n'); await waitText(pid, 'Started user process');
        const spin = (await processes()).find(p => p.name === 'spin.snx');
        assert(spin, 'detached process alive');
        const unrelated = await launch('spin');
        await type('kill ' + unrelated + '\n'); await waitText(pid, 'start failed -3');
        assert((await processes()).some(p => p.pid === unrelated), 'unrelated process remains alive');
        await command('kill ' + unrelated);
        await type('kill ' + spin.pid + '\n');
        await until(async () => (await processes()).every(p => p.name !== 'spin.snx'), 'detached process reaped');
        await key('ctrl','w');
        await until(async () => (await processes()).length === 0, 'F2 session closed');
        await command('rm /home/user/g6-test.txt');
        assert.deepEqual(await resources(), baseline);
    });
    await check('G2 desktop broker restores foreground after exit, fault and cancel', async () => {
        first=await launch('termqa','/broker-test');await waitText(first,'G2 ready');
        await key('ctrl','c');await waitText(first,'G2 interrupt');
        await type('go\n');await waitText(first,'G2 restored');
        const before=await value('process_faults');
        await type('fault\n');
        await until(async()=>await value('process_faults')===before+1,'G2 child fault');
        await until(async()=>((await text(first)).match(/G2 restored/g)||[]).length>=2,'restore after fault');
        await type('spin\n');await waitText(first,'G2 worker waiting');
        second=await launch('termqa','/broker-test');await waitText(second,'G2 ready');
        await type('spin\n');await waitText(second,'G2 worker waiting');
        await key('ctrl','c');await waitText(second,'G2 restored');
        assert((await processes()).some(p=>p.pid===first));
        assert.equal((await processes()).filter(p=>p.name==='brokerqa.snx').length,3,'other foreground task survives');
        await key('ctrl','w');await until(async()=>await window(second)<0,'second G2 closes');
        await key('ctrl','w');await until(async()=>(await processes()).length===0,'G2 group reaped');
        assert.deepEqual(await resources(),baseline);
    });
    await check('G2 last controller loss cancels noncooperative owned tasks', async () => {
        const before=await value('process_faults');
        first=await launch('termqa','/broker-fault');await waitText(first,'G2 ready');
        await type('spin\n');
        await until(async()=>await value('process_faults')===before+1,'G2 frontend fault');
        await until(async()=>(await processes()).length===0,'kernel session ownership cleanup');
        assert.deepEqual(await resources(),baseline);
    });

}
module.exports = {run};
